import os
import shutil
import socket
import subprocess
import sys
import threading
import time

import redis

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "build", "minidb_server.exe")
DATA = os.path.join(ROOT, "testdata", "resp")
LOG = os.path.join(ROOT, "build", "resp_server.log")
HOST = "127.0.0.1"

failures = []


class ErrorReply:
    def __init__(self, text):
        self.text = text

    def __repr__(self):
        return "ErrorReply(" + repr(self.text) + ")"

    def __eq__(self, other):
        return isinstance(other, ErrorReply) and self.text == other.text

    def __hash__(self):
        return hash(self.text)


class RawClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buffer = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    @staticmethod
    def encode(*args):
        parts = [b"*" + str(len(args)).encode() + b"\r\n"]
        for arg in args:
            if isinstance(arg, str):
                arg = arg.encode()
            parts.append(b"$" + str(len(arg)).encode() + b"\r\n" + arg + b"\r\n")
        return b"".join(parts)

    def send(self, *args):
        self.sock.sendall(self.encode(*args))

    def read_line(self):
        while b"\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed while reading line")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def read_exact(self, size):
        while len(self.buffer) < size:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed while reading body")
            self.buffer += chunk
        data = self.buffer[:size]
        self.buffer = self.buffer[size:]
        return data

    def read_reply(self):
        line = self.read_line()
        marker = line[:1]
        body = line[1:]
        if marker == b"+":
            return body
        if marker == b"-":
            return ErrorReply(body.decode("utf-8", "replace"))
        if marker == b":":
            return int(body)
        if marker == b"$":
            length = int(body)
            if length < 0:
                return None
            data = self.read_exact(length)
            self.read_exact(2)
            return data
        if marker == b"*":
            count = int(body)
            if count < 0:
                return None
            return [self.read_reply() for _ in range(count)]
        raise RuntimeError("unexpected reply marker " + repr(marker))

    def command(self, *args):
        self.send(*args)
        return self.read_reply()


def check(condition, label, detail=""):
    if condition:
        print("[PASS] " + label)
        return
    failures.append(label)
    suffix = "" if not detail else ": " + detail
    print("[FAIL] " + label + suffix)


def check_equal(actual, expected, label):
    check(actual == expected, label,
          "expected " + repr(expected) + ", got " + repr(actual))


def pick_port():
    override = os.environ.get("MINIDB_RESP_PORT")
    if override:
        return int(override)
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind((HOST, 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def start_server(port, reset):
    if reset and os.path.isdir(DATA):
        shutil.rmtree(DATA)
    if not os.path.isdir(DATA):
        os.makedirs(DATA)
    log = open(LOG, "ab")
    proc = subprocess.Popen(
        [SERVER, "--data", DATA, "--port", str(port), "--threads", "8",
         "--memtable-kb", "64", "--l0-trigger", "3", "--level-base-kb", "128",
         "--target-kb", "128"],
        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    log.close()
    deadline = time.time() + 20.0
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("server exited with code " + str(proc.returncode))
        try:
            probe = RawClient(HOST, port)
            reply = probe.command("PING")
            probe.close()
            if reply == b"PONG":
                return proc
        except (OSError, RuntimeError):
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready on port " + str(port))


def shutdown_server(port, proc):
    client = RawClient(HOST, port)
    try:
        reply = client.command("SHUTDOWN")
        check_equal(reply, b"OK", "SHUTDOWN reply")
    finally:
        client.close()
    proc.wait(timeout=20)
    return proc.returncode


def connect(port, decode):
    return redis.Redis(host=HOST, port=port, decode_responses=decode, protocol=2,
                       socket_connect_timeout=5, socket_timeout=5)


def test_core_commands(port):
    client = connect(port, True)
    binary = connect(port, False)
    try:
        check_equal(client.ping(), True, "redis-py PING")
        check_equal(client.set("smoke:key", "value-1"), True, "redis-py SET")
        check_equal(client.get("smoke:key"), "value-1", "redis-py GET")
        check_equal(client.get("smoke:missing"), None, "GET missing key returns nil")
        check_equal(client.exists("smoke:key"), 1, "EXISTS present key")
        check_equal(client.delete("smoke:key"), 1, "DEL present key")
        check_equal(client.get("smoke:key"), None, "GET after DEL returns nil")
        check_equal(client.delete("smoke:key"), 0, "DEL missing key returns zero")
        check_equal(client.mset({"smoke:a": "1", "smoke:b": "2"}), True, "MSET")
        check_equal(client.mget("smoke:a", "smoke:b", "smoke:none"), ["1", "2", None],
                    "MGET mixed keys")
        check_equal(client.type("smoke:a"), "string", "TYPE string key")
        check_equal(client.type("smoke:none"), "none", "TYPE missing key")
        check_equal(client.set("smoke:nx", "first", nx=True), True, "SET NX on missing key")
        check_equal(client.set("smoke:nx", "second", nx=True), None, "SET NX on existing key")
        check_equal(client.get("smoke:nx"), "first", "SET NX preserved first value")
        check_equal(client.set("smoke:absent", "value", xx=True), None, "SET XX on missing key")
        check_equal(client.set("smoke:nx", "third", get=True), "first",
                    "SET GET returns previous value")
        check_equal(client.get("smoke:nx"), "third", "SET GET stored new value")
        binary_key = b"binary\x00key\r\n"
        binary_value = b"\x00\x01\r\n\xffbinary-value"
        check_equal(binary.set(binary_key, binary_value), True, "binary SET")
        check_equal(binary.get(binary_key), binary_value, "binary GET")
        check_equal(binary.delete(binary_key), 1, "binary DEL")
        empty_value = b""
        check_equal(binary.set(b"empty:value", empty_value), True, "empty value SET")
        check_equal(binary.get(b"empty:value"), empty_value, "empty value GET")
    finally:
        client.close()
        binary.close()


def test_pipeline_transaction(port):
    client = connect(port, True)
    try:
        pipe = client.pipeline(transaction=True)
        pipe.set("tx:pipeline:a", "1")
        pipe.set("tx:pipeline:b", "2")
        pipe.get("tx:pipeline:a")
        result = pipe.execute()
        check_equal(result, [True, True, "1"], "redis-py MULTI/EXEC result array")
        check_equal(client.get("tx:pipeline:a"), "1", "transaction first value visible")
        check_equal(client.get("tx:pipeline:b"), "2", "transaction second value visible")
    finally:
        client.close()


def test_raw_transactions(port):
    client = RawClient(HOST, port)
    try:
        check_equal(client.command("MULTI"), b"OK", "raw MULTI reply")
        check_equal(client.command("SET", "tx:raw:a", "alpha"), b"QUEUED", "raw queued SET")
        check_equal(client.command("GET", "tx:raw:a"), b"QUEUED", "raw queued GET")
        check_equal(client.command("DEL", "tx:raw:missing"), b"QUEUED", "raw queued DEL")
        check_equal(client.command("EXEC"), [b"OK", b"alpha", 0],
                    "raw EXEC result array")

        check_equal(client.command("MULTI"), b"OK", "raw MULTI before DISCARD")
        check_equal(client.command("SET", "tx:discard", "gone"), b"QUEUED",
                    "raw queued SET before DISCARD")
        check_equal(client.command("DISCARD"), b"OK", "raw DISCARD reply")
        check_equal(client.command("EXEC"), ErrorReply("ERR EXEC without MULTI"),
                    "EXEC after DISCARD reports error")
        check_equal(client.command("GET", "tx:discard"), None,
                    "DISCARD dropped queued write")

        check_equal(client.command("MULTI"), b"OK", "raw MULTI before nested MULTI")
        nested = client.command("MULTI")
        check(isinstance(nested, ErrorReply) and "nested" in nested.text,
              "nested MULTI rejected", repr(nested))
        check_equal(client.command("SET", "tx:nested", "kept"), b"QUEUED",
                    "transaction survives nested MULTI error")
        check_equal(client.command("EXEC"), [b"OK"],
                    "EXEC after nested MULTI error")

        check_equal(client.command("MULTI"), b"OK", "raw MULTI before unknown command")
        unknown = client.command("NOTACOMMAND", "x")
        check(isinstance(unknown, ErrorReply), "unknown command rejected while queuing",
              repr(unknown))
        abort = client.command("EXEC")
        check(isinstance(abort, ErrorReply) and abort.text.startswith("EXECABORT"),
              "EXEC aborts after queue error", repr(abort))

        check_equal(client.command("MULTI"), b"OK", "raw MULTI before pipelined block")
        payload = (RawClient.encode("SET", "tx:pipe", "9") +
                   RawClient.encode("GET", "tx:pipe") +
                   RawClient.encode("EXEC"))
        client.sock.sendall(payload)
        replies = [client.read_reply() for _ in range(3)]
        check_equal(replies, [b"QUEUED", b"QUEUED", [b"OK", b"9"]],
                    "pipelined transaction replies")

        split_payload = RawClient.encode("SET", "partial:key", "partial-value")
        client.sock.sendall(split_payload[:9])
        time.sleep(0.05)
        client.sock.sendall(split_payload[9:])
        check_equal(client.read_reply(), b"OK", "split frame reassembly")
        check_equal(client.command("GET", "partial:key"), b"partial-value",
                    "split frame value readable")

        client.sock.sendall(b"SET inline:key inline-value\r\n")
        check_equal(client.read_reply(), b"OK", "inline SET compatibility")
        client.sock.sendall(b"GET inline:key\r\n")
        check_equal(client.read_reply(), b"inline-value", "inline GET compatibility")
        client.sock.sendall(b"PUT legacy:key legacy-value\r\n")
        check_equal(client.read_reply(), b"OK", "legacy PUT compatibility")
        check_equal(client.command("GET", "legacy:key"), b"legacy-value",
                    "legacy PUT readable through RESP")
    finally:
        client.close()


def isolation_writer(port, iterations, errors):
    client = connect(port, True)
    try:
        for index in range(iterations):
            pipe = client.pipeline(transaction=True)
            pipe.set("isolation:a", str(index))
            pipe.set("isolation:b", str(index))
            result = pipe.execute()
            if result != [True, True]:
                errors.append("writer result " + repr(result))
                return
    except Exception as error:
        errors.append("writer error " + repr(error))
    finally:
        client.close()


def isolation_reader(port, stop_event, errors):
    client = connect(port, True)
    try:
        while not stop_event.is_set():
            values = client.mget("isolation:a", "isolation:b")
            if values[0] is not None and values[0] != values[1]:
                errors.append("torn read " + repr(values))
                return
            time.sleep(0.001)
    except Exception as error:
        errors.append("reader error " + repr(error))
    finally:
        client.close()


def test_isolation(port):
    errors = []
    stop_event = threading.Event()
    reader = threading.Thread(target=isolation_reader, args=(port, stop_event, errors))
    reader.start()
    isolation_writer(port, 100, errors)
    stop_event.set()
    reader.join(timeout=10)
    check_equal(errors, [], "transaction isolation under concurrent reader")
    client = connect(port, True)
    try:
        check_equal(client.mget("isolation:a", "isolation:b"), ["99", "99"],
                    "final isolation values")
    finally:
        client.close()


def test_persistence(port, proc):
    check_equal(shutdown_server(port, proc), 0, "server exit code after SHUTDOWN")
    restarted = start_server(port, False)
    client = connect(port, True)
    binary = connect(port, False)
    try:
        check_equal(client.get("tx:pipeline:a"), "1", "transaction value after restart")
        check_equal(client.get("tx:pipeline:b"), "2", "transaction second value after restart")
        check_equal(client.get("tx:raw:a"), "alpha", "raw transaction value after restart")
        check_equal(client.get("tx:nested"), "kept", "nested MULTI transaction after restart")
        check_equal(binary.get(b"empty:value"), b"", "empty value after restart")
        check_equal(client.get("isolation:a"), "99", "isolation value after restart")
        check_equal(client.ping(), True, "restarted server responds to PING")
    finally:
        client.close()
        binary.close()
    check_equal(shutdown_server(port, restarted), 0, "restarted server exit code")
    return restarted


def main():
    port = pick_port()
    proc = None
    try:
        proc = start_server(port, True)
        print("[STAGE] RESP server ready on " + HOST + ":" + str(port))
        test_core_commands(port)
        test_pipeline_transaction(port)
        test_raw_transactions(port)
        test_isolation(port)
        proc = test_persistence(port, proc)
    except Exception as error:
        failures.append("fatal " + repr(error))
        print("[FAIL] fatal: " + repr(error))
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    if failures:
        print("[RESULT] RESP test failed: " + str(len(failures)) + " check(s)")
        return 1
    print("[RESULT] RESP test passed: redis-py and raw RESP checks all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
