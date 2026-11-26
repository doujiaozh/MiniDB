# MiniDB

Windows 7 上从零实现的极简持久化键值存储引擎，C++17 编写，对外提供 C API。

## 目标

- 内存索引使用跳表，读写路径无全局写锁竞争
- 所有写操作先追加预写日志，进程崩溃后可完整恢复
- 内存表达到阈值后冻结并落盘为 SSTable，读取使用块索引与块内二分查找
- 每个 SSTable 携带布隆过滤器，快速排除不存在的键
- 后台线程合并 SSTable，回收墓碑与旧版本
- 提供 db_put、db_get、db_delete 三个核心 C 接口

## 目录结构

    include/minidb.h        公开 C API
    src/util.h/.cpp         CRC32、64 位哈希、定长编码
    src/platform.h/.cpp     Windows 文件与目录操作、定位读写、原子重命名
    src/skiplist.h/.cpp     跳表内存索引
    src/bloom.h/.cpp        布隆过滤器
    src/wal.h/.cpp          预写日志
    src/sstable.h/.cpp      SSTable 构建与读取
    src/db_impl.h/.cpp      引擎核心、恢复、刷盘、压缩、C API
    tests/test_persist.cpp  崩溃恢复与压缩验证
    tests/test_bench.cpp    10000 次随机读写正确性与性能
    tests/test_concurrent.cpp 多线程读写一致性
    build.bat               MinGW 构建脚本
    Makefile                mingw32-make 构建脚本

## 架构

写入路径：

1. 分配单调递增序号，追加 WAL 记录（长度、CRC32、序号、类型、键、值）
2. 插入跳表，节点按 键升序、序号降序 排列，因此同一键的最新版本总在最前
3. 跳表内存占用达到阈值时冻结为不可变内存表，切换到新 WAL，落盘 SSTable，更新清单

读取路径：

1. 依次查询可变内存表、不可变内存表，再从新到旧查询 SSTable
2. SSTable 先查布隆过滤器，未命中直接返回
3. 命中则用块最大键做二分查找定位数据块，再在块内用偏移表二分查找键
4. 首个命中的版本即最新版本；墓碑返回不存在

压缩：

1. 内存表落盘后通知后台线程
2. 活跃 SSTable 数量达到阈值时，后台线程对所有表做多路归并
3. 每个键只保留序号最大的版本，墓碑与旧版本丢弃
4. 新表落盘并同步，原子更新 MANIFEST，再删除输入文件

## 磁盘格式

SSTable 布局：

    [数据块 0] [数据块 1] ... [索引块] [布隆块] [尾部 56 字节]

- 数据块：块内条目数、每条的块内偏移数组、条目（键长、值长、序号、类型、键、值）
- 索引块：每块的最大键、文件偏移、块长度，读取时按最大键二分
- 尾部：索引偏移与长度、布隆偏移与长度、条目数、最大序号、魔数

WAL 记录：

    [载荷长度 4][CRC32 4][序号 8][类型 1][键长 4][值长 4][键][值]

MANIFEST 采用文本格式，列出活跃 SSTable 编号与已持久化最大序号，通过 写临时文件 + 同步 + 原子重命名 更新。恢复时以 MANIFEST 为准，未被引用的 SSTable 视为崩溃残留并删除。

## ACID 说明

- 原子性：单键写入是一个 WAL 记录，恢复时整条记录校验通过才生效，截断的尾部记录被丢弃
- 一致性：序号决定版本顺序，压缩保留最新版本并丢弃墓碑
- 隔离性：读取持有版本快照，冻结内存表与 SSTable 在引用释放前不会被回收
- 持久性：WAL 记录逐条写入操作系统；开启 sync_on_write 后每次写入调用 FlushFileBuffers；SSTable 与 MANIFEST 在发布前执行同步

## 构建

    build.bat

或

    mingw32-make

产物位于 build 目录：libminidb.a、test_persist.exe、test_bench.exe、test_concurrent.exe。

## 运行

    build\test_persist.exe
    build\test_bench.exe 64
    build\test_bench.exe 4096
    build\test_concurrent.exe

test_bench 的参数为内存表大小，单位 KB。

## 验证结果

测试环境：Windows 7 SP1，MinGW-w64 GCC 8.1.0，单机械硬盘，4 核。

持久化与崩溃恢复：

- 子进程写入 500 条、覆盖 100 条、删除 167 条后直接退出，不关闭数据库
- 重启后 333 条存活记录全部恢复，167 条删除记录未复活，覆盖值正确
- 再次正常关闭并重启，结果一致
- 4000 条写入加 2000 条删除，后台压缩自动触发；强制压缩后仅剩 1 个 SSTable、2000 条记录，墓碑与旧版本被回收；重启后数据一致

10000 次随机读写（2000 键，60 百分比写、25 百分比读、15 百分比删除）：

- 64KB 内存表压力配置：13 次刷盘、5 次压缩，吞吐 7080 ops/s，随机读写全部匹配参考模型
- 4MB 默认配置：吞吐 340892 ops/s，随机读写全部匹配参考模型
- 关闭后重新打开：10000 次随机读加 2000 键全量扫描，吞吐 320256 gets/s，结果全部匹配
- 两种配置下参考模型比对均为零误差

并发：

- 4 写线程加 4 读线程，90000 次操作，吞吐 81163 ops/s
- 零写失败、零读失败、零撕裂读、零跨键污染，结束后全量校验一致

## C API

    void db_options_init(minidb_options* options);
    minidb* db_open(const char* path, const minidb_options* options);
    int db_put(minidb* db, const char* key, size_t key_len, const char* value, size_t value_len);
    int db_get(minidb* db, const char* key, size_t key_len, char** value, size_t* value_len);
    void db_free_value(char* value);
    int db_delete(minidb* db, const char* key, size_t key_len);
    int db_flush(minidb* db);
    int db_compact(minidb* db);
    void db_get_stats(minidb* db, minidb_stats* stats);
    const char* db_last_error(minidb* db);
    int db_close(minidb* db);

返回码：DB_OK 为 0，DB_NOT_FOUND 为 1，DB_ERR 为 -1。db_get 返回的缓冲区由调用方通过 db_free_value 释放。

## 边界

- 单进程写，写操作由进程内互斥量串行化
- 压缩为全量归并，未实现分层或增量压缩
- 未实现快照读与多版本事务
- 键与值以字节串处理，允许二进制内容
