# MiniDB

Windows 7 上从零实现的持久化键值存储引擎与 TCP 服务端，C++17 编写，对外提供 C API，以及兼容 Redis RESP2 的网络接口。

## 目标

- 内存索引使用跳表，读写路径无全局写锁竞争
- 所有写操作先追加预写日志，进程崩溃后可完整恢复
- 内存表达到阈值后冻结并落盘为 SSTable，读取使用块索引与块内二分查找
- 每个 SSTable 携带布隆过滤器，快速排除不存在的键
- 分层压缩 L0 至 L3，降低写放大，L0 允许重叠，L1 以上键全局有序且不重叠
- TCP 服务端监听 6379，工作线程池处理并发连接，RESP2 支持 SET、GET、DEL 与 MULTI、EXEC、DISCARD 基础事务
- 保留原有空格分隔的行内指令，旧的 PUT、GET、DEL、PUTB 调用方式继续可用
- 提供命令行客户端与并发网络测试

## 目录结构

    include/minidb.h          公开 C API
    src/util.h/.cpp           CRC32、64 位哈希、定长编码
    src/platform.h/.cpp       Windows 文件与目录操作、定位读写、原子重命名
    src/skiplist.h/.cpp       跳表内存索引
    src/bloom.h/.cpp          布隆过滤器
    src/wal.h/.cpp            预写日志
    src/sstable.h/.cpp        SSTable 构建与读取
    src/db_impl.h/.cpp        引擎核心、分层压缩、恢复、C API
    src/resp.h                RESP2 编解码与增量请求解析
    src/server.cpp            RESP 服务端、事务队列与线程池
    src/cli.cpp               命令行客户端
    src/net_client.h          Winsock 客户端与协议编解码
    tests/test_persist.cpp    崩溃恢复与压缩验证
    tests/test_bench.cpp      10000 次随机读写正确性与性能
    tests/test_concurrent.cpp 多线程读写一致性
    tests/test_levels.cpp     分层压缩、层间有序性、墓碑遮蔽
    tests/test_network.cpp    并发客户端网络读写与协议验证
    tests/test_resp.py        redis-py 与原始 RESP 事务兼容性验证
    build.bat                 MinGW 构建脚本
    Makefile                  mingw32-make 构建脚本
    run_network_test.bat      一键启动服务端并执行网络测试
    run_resp_test.bat         一键执行 redis-py RESP 测试

## 写入与读取路径

写入：

1. 分配单调递增序号，追加 WAL 记录（长度、CRC32、序号、类型、键、值）
2. 插入跳表，节点按 键升序、序号降序 排列，同一键的最新版本在最前
3. 跳表内存占用达到阈值时冻结为不可变内存表，切换到新 WAL，落盘为 L0 的 SSTable，原子更新清单

读取：

1. 依次查询可变内存表、不可变内存表
2. 从 L0 到 L3 逐层查询；L0 内按文件编号从新到旧扫描，L1 以上按最小键二分定位唯一候选文件
3. 每个 SSTable 先查布隆过滤器，命中则块索引二分、块内偏移表二分
4. 首个命中版本即最新版本，墓碑返回不存在

## 分层压缩

- L0 由刷盘直接产生，文件之间键范围允许重叠，内存表大小阈值决定刷盘频率
- L1 到 L3 的键范围全局有序且不重叠，恢复与压缩后都按最小键排序
- L0 文件数达到阈值时，合并全部 L0 与 L1 中范围重叠的文件，输出到 L1，按目标文件大小在键边界切分
- L1、L2 的总字节数超过容量限制时，选取该层首个文件，与下一层重叠文件归并后写入下一层
- L1 容量为基准值，之后每层乘以层倍数；L3 为最底层，容量不设限
- 只有写入最底层时才丢弃墓碑与旧版本，其余层级保留墓碑以保证跨层遮蔽正确
- 清单记录每个文件所属层级，崩溃恢复按层级重建版本视图，未被清单引用的 SSTable 视为残留并删除

## TCP 服务端

- 监听 0.0.0.0:6379，主线程使用 select 轮询接受连接
- 固定大小工作线程池，每个连接由工作线程处理到 EOF，网络读写与引擎并发控制相互独立
- 增量 RESP2 解析器支持数组请求与行内请求，支持流水线与半包重组
- 服务端直接调用 C API，写操作由引擎内部的写互斥量串行化，读操作持有版本快照
- 服务端读写锁保证 EXEC 期间不与其它连接的数据指令交错
- 连接级接收发送超时 120 秒，行内请求上限 1MB，单个批量值上限 64MB，挂起缓冲上限 256MB

启动：

    build\minidb_server.exe --data testdata\server --port 6379 --threads 32
                           --memtable-kb 64 --l0-trigger 3 --level-base-kb 128 --target-kb 128

关闭：发送 SHUTDOWN 指令，服务端回复 OK 后停止接收连接，排空工作线程并刷盘退出。

## 协议

服务端兼容 Redis RESP2。标准客户端发送数组请求，响应使用简单字符串 +、错误 -、整数 :、批量字符串 $ 与数组 *。

请求示例：

    *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n

基础指令：

    PING [message]                 +PONG 或批量回显
    ECHO message                   $len CRLF message CRLF
    SET key value [NX|XX] [GET]    +OK，条件不满足返回 $-1，GET 返回旧值
    GET key                        $len CRLF value CRLF，缺失返回 $-1
    DEL key [key ...]              :删除的键数量
    EXISTS key [key ...]           :存在的键数量
    MGET key [key ...]             *N，逐项批量字符串或空值
    MSET key value [key value ...] +OK
    TYPE key                       +string 或 +none
    SELECT 0                       +OK
    CLIENT SETINFO|SETNAME|GETNAME|ID|INFO  连接元数据
    COMMAND [subcommand]           *0
    INFO [section]                 $len CRLF 统计文本 CRLF
    STATS                          $len CRLF 键值统计文本 CRLF
    LEVELS                         $len CRLF 每行层级、文件号、条目数、最小键十六进制、最大键十六进制
    CHECK                          +OK 表示各层有序且不重叠
    FLUSH                          +OK
    COMPACT                        +OK，全量压缩到最底层
    QUIT                           +OK 并关闭连接
    SHUTDOWN                       +OK 并停止服务

事务指令：

    MULTI                          +OK，开启连接级事务队列
    EXEC                           *N，按序返回队列中每个指令的响应
    DISCARD                        +OK，清空事务队列

行内兼容形式：

    PUT key value                  +OK
    PUTB klen vlen CRLF key value  +OK
    GET key                        $len CRLF value CRLF，缺失返回 $-1
    DEL key                        :1

行内形式的键值不能包含空格，二进制安全的写入使用 PUTB 或 RESP 批量字符串。

## 事务

- MULTI 之后的数据指令不立即执行，统一返回 +QUEUED
- EXEC 在服务端全局排他锁内按队列顺序执行，响应为与队列等长的数组，执行期间其它连接无法穿插
- DISCARD 丢弃整个队列并退出事务状态
- 队列阶段遇到未知指令或参数错误时记录事务脏标记，EXEC 返回 -EXECABORT 并丢弃队列
- 嵌套 MULTI 返回错误但不脏化事务；未开启事务时 EXEC 与 DISCARD 返回错误
- 事务只提供隔离与按序执行，不支持回滚与 WATCH 条件事务，WAL 仍按单条指令追加

## 命令行客户端

    build\minidb_cli.exe -h 127.0.0.1 -p 6379 PUT key value
    build\minidb_cli.exe -h 127.0.0.1 -p 6379 GET key
    build\minidb_cli.exe -h 127.0.0.1 -p 6379 LEVELS
    build\minidb_cli.exe -h 127.0.0.1 -p 6379

不带指令时进入交互模式，输入 quit 或 exit 退出。

## 构建

    build.bat

或

    mingw32-make

产物位于 build 目录：libminidb.a、minidb_server.exe、minidb_cli.exe、test_persist.exe、test_bench.exe、test_concurrent.exe、test_levels.exe、test_network.exe。

## 运行验证

    build\test_levels.exe
    build\test_persist.exe
    build\test_bench.exe 64
    build\test_concurrent.exe
    run_network_test.bat
    run_resp_test.bat

网络测试脚本会以分层压力配置启动服务端，等待就绪后执行 16 个并发客户端共 10000 次混合读写，结束时发送 SHUTDOWN。

RESP 测试脚本自行选择空闲端口、启动服务端、执行 redis-py 与原始 RESP 校验，并在验证重启持久化后关闭服务端。

## 验证结果

测试环境：Windows 7 SP1，MinGW-w64 GCC 8.1.0，单机械硬盘，4 核。

分层压缩：

- 8000 条写入后层级分布为 L0 3 个、L1 1 个、L2 5 个、L3 2 个文件，CHECK 通过，L1 以上层内有序不重叠
- 删除三分之一键后墓碑在 L0 与内存表遮蔽深层旧值，重启后删除状态保持
- 强制全量压缩后全部文件进入 L3，共 5 个文件、5333 条有效记录，墓碑与旧版本被回收
- 重启后层级分布与数据完全一致，CHECK 通过

网络路径（16 并发客户端，10000 次混合读写，分层压力配置 64KB 内存表）：

- 写入 7077 次、读取 1944 次、删除 979 次，零客户端失败，零数据不一致
- 网络校验 2000 个键全部匹配参考模型，其中 1700 个存活、300 个已删除
- 服务端层级统计为 L0 2 个、L1 1 个，压缩计数 4，CHECK 返回 OK
- 协议校验通过：缺失键返回空值、未知指令返回错误、参数错误返回错误、PING 正常、STATS 含层级字段、LEVELS 层内有序、RESP 批量字符串二进制值往返一致
- 吞吐 2962 ops/s，受刷盘与压缩同步影响
- 1MB 内存表配置下同一负载吞吐 23848 ops/s

RESP 与事务（python redis-py 8.2.0，独立空闲端口，全部校验通过）：

- redis-py 连接成功，PING、SET、GET、DEL、EXISTS、MSET、MGET、TYPE、SET NX/XX/GET 全部符合 Redis 语义
- 含空字节与 CRLF 的二进制键值、空值经 RESP 批量字符串往返一致
- redis-py 流水线事务返回 [True, True, "1"]，原始 RESP 的 MULTI/EXEC 返回 [OK, alpha, 0]
- DISCARD 丢弃队列，未开启事务的 EXEC/DISCARD 报错，未知指令使 EXEC 返回 EXECABORT，嵌套 MULTI 报错且不脏化事务
- 流水线、半包与行内兼容指令解析正确，旧 PUT/GET/PUTB 调用方式继续可用
- 100 个事务执行期间并发读线程零撕裂读，事务写入对外原子可见
- SHUTDOWN 退出码 0，重启后事务写入、空值与隔离测试的最终值完整恢复

网络持久化：

- 测试写入哨兵键后 SHUTDOWN，服务端退出码 0
- 重启服务端后哨兵值完整读回，LEVELS 显示 L0 1 个、L1 1 个文件，CHECK 通过

引擎：

- 崩溃恢复：子进程写入 500 条、覆盖 100 条、删除 167 条后直接退出，重启后 333 条存活记录全部恢复，删除记录零复活
- 10000 次随机读写：64KB 内存表配置 5180 ops/s、13 次刷盘、4 次压缩；4MB 配置 212734 ops/s；参考模型零误差
- 重开后 10000 次随机读加 2000 键扫描吞吐 225151 gets/s，零误差
- 并发：4 写线程加 4 读线程 90000 次操作，零撕裂读、零跨键污染

## C API

    void db_options_init(minidb_options* options);
    minidb* db_open(const char* path, const minidb_options* options);
    int db_put(minidb* db, const char* key, size_t key_len, const char* value, size_t value_len);
    int db_get(minidb* db, const char* key, size_t key_len, char** value, size_t* value_len);
    void db_free_value(char* value);
    int db_delete(minidb* db, const char* key, size_t key_len);
    int db_flush(minidb* db);
    int db_compact(minidb* db);
    int db_verify_levels(minidb* db);
    int db_levels_text(minidb* db, char** text, size_t* text_len);
    void db_get_stats(minidb* db, minidb_stats* stats);
    const char* db_last_error(minidb* db);
    int db_close(minidb* db);

返回码：DB_OK 为 0，DB_NOT_FOUND 为 1，DB_ERR 为 -1。db_get 与 db_levels_text 返回的缓冲区由调用方通过 db_free_value 释放。

## ACID 说明

- 原子性：单键写入是一个 WAL 记录，恢复时整条记录校验通过才生效，截断的尾部记录被丢弃
- 一致性：序号决定版本顺序，压缩只保留最新版本，墓碑仅在底层丢弃
- 隔离性：读取持有版本快照，冻结内存表与 SSTable 在引用释放前不会被回收；EXEC 持有服务端排他锁，事务内多条指令对外表现为不可分割的执行序列
- 持久性：WAL 记录逐条写入操作系统；开启 sync_on_write 后每次写入调用 FlushFileBuffers；SSTable 与清单在发布前执行同步

## 边界

- 单进程写，写操作由进程内互斥量串行化
- 压缩为逐层归并，未实现并发压缩与增量优先级调度
- 事务支持 MULTI、EXEC、DISCARD，不实现 WATCH、回滚与 RESP3
- 未实现键过期（SET EX/PX）、SCAN 与主从复制
- 键与值以字节串处理，允许二进制内容，网络行内指令不支持含空格的键值，网络层拒绝空键
