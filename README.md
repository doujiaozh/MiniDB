# MiniDB

Windows 7 上从零实现的持久化键值存储引擎与 TCP 服务端，C++17 编写，对外提供 C API 与文本协议接口。

## 目标

- 内存索引使用跳表，读写路径无全局写锁竞争
- 所有写操作先追加预写日志，进程崩溃后可完整恢复
- 内存表达到阈值后冻结并落盘为 SSTable，读取使用块索引与块内二分查找
- 每个 SSTable 携带布隆过滤器，快速排除不存在的键
- 分层压缩 L0 至 L3，降低写放大，L0 允许重叠，L1 以上键全局有序且不重叠
- TCP 服务端监听 6379，工作线程池处理并发连接，文本协议支持 PUT、GET、DEL
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
    src/server.cpp            TCP 服务端与线程池
    src/cli.cpp               命令行客户端
    src/net_client.h          Winsock 客户端与协议编解码
    tests/test_persist.cpp    崩溃恢复与压缩验证
    tests/test_bench.cpp      10000 次随机读写正确性与性能
    tests/test_concurrent.cpp 多线程读写一致性
    tests/test_levels.cpp     分层压缩、层间有序性、墓碑遮蔽
    tests/test_network.cpp    并发客户端网络读写与协议验证
    build.bat                 MinGW 构建脚本
    Makefile                  mingw32-make 构建脚本
    run_network_test.bat      一键启动服务端并执行网络测试

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
- 服务端直接调用 C API，写操作由引擎内部的写互斥量串行化，读操作持有版本快照
- 连接级接收发送超时 120 秒，单行上限 1MB，批量值上限 64MB

启动：

    build\minidb_server.exe --data testdata\server --port 6379 --threads 32
                           --memtable-kb 64 --l0-trigger 3 --level-base-kb 128 --target-kb 128

关闭：发送 SHUTDOWN 指令，服务端回复 OK 后停止接收连接，排空工作线程并刷盘退出。

## 协议

请求以 CRLF 结尾，响应采用整数、简单字符串、批量字符串或错误四种形式。

    PUT key value                  +OK
    PUTB klen vlen CRLF key value  +OK
    GET key                        $len CRLF value CRLF，缺失返回 $-1
    DEL key                        :1
    PING                           +PONG
    STATS                          $len CRLF 键值统计文本 CRLF
    LEVELS                         $len CRLF 每行层级、文件号、条目数、最小键十六进制、最大键十六进制
    CHECK                          +OK 表示各层有序且不重叠
    FLUSH                          +OK
    COMPACT                        +OK，全量压缩到最底层
    SHUTDOWN                       +OK 并停止服务

PUT 的键值不能包含空格，二进制安全的写入使用 PUTB。

命令行客户端：

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

网络测试脚本会以分层压力配置启动服务端，等待就绪后执行 16 个并发客户端共 10000 次混合读写，结束时发送 SHUTDOWN。

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
- 服务端层级统计为 L0 0 至 1 个、L1 1 个，压缩计数 2，CHECK 返回 OK
- 协议校验通过：缺失键返回空值、未知指令返回错误、参数错误返回错误、PING 正常、STATS 含层级字段、LEVELS 层内有序、PUTB 二进制值往返一致
- 吞吐 1531 至 6609 ops/s，受刷盘与压缩同步影响
- 1MB 内存表配置下同一负载吞吐 23848 ops/s

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
- 隔离性：读取持有版本快照，冻结内存表与 SSTable 在引用释放前不会被回收
- 持久性：WAL 记录逐条写入操作系统；开启 sync_on_write 后每次写入调用 FlushFileBuffers；SSTable 与清单在发布前执行同步

## 边界

- 单进程写，写操作由进程内互斥量串行化
- 压缩为逐层归并，未实现并发压缩与增量优先级调度
- 未实现快照读、多版本事务与主从复制
- 键与值以字节串处理，允许二进制内容，网络内联指令不支持含空格的键值
