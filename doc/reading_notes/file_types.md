# LevelDB 中各种文件的作用

在 LevelDB 中，有以下几种类型的文件:

- SST(Sorted String Table) 文件名为`xxx.ldb`，存储实际的 Key-Value 数据。

- WAL(Write Ahead Log) 文件名为`xxx.log`，记录最近的写操作，以便在系统崩溃后恢复数据。

- MANIFEST 文件名为`MANIFEST-xxx`，记录了数据库的当前状态。

- CURRENT 文件名为`CURRENT`，指向当前使用的 MANIFEST 文件，用于快速定位数据库状态

- LOCK 文件名为`LOCK`，防止数据库被多个进程同时访问

- LOG 文件名为`LOG`，日志输出

## SST

SST 文件是 LevelDB 中存储实际 Key-Value 数据的文件。

## WAL

当我们往 LevelDB 中写入一对 Key-Value 时，其大概流程是:

1. 会往 WAL 中写入一条记录，我们可以简单表示成 `Add Key: key_a, Value: value_a`。
2. 将这对 Key-Value 写入内存中的 MemTable。

当内存中的 MemTable 大小达到阈值时，会将 MemTable 写入到磁盘，变成 SST 文件。

倘若我们将 Key-Value 写入 MemTable 后，在 MemTable 写入磁盘之前，系统崩溃了，那么这条记录就会丢失。为了防止这种情况，LevelDB 会将每一条写操作都记录到 WAL 中，这样即使系统崩溃，我们也可以通过 WAL 来恢复 MemTable 中的数据，保证数据不丢失。

## MANIFEST

如何理解 MANIFEST 记录了数据的当前状态呢？

其实就是记录了当前 LevelDB 中有哪些 SST 文件，每个 SST 的大小，SST 里 Key 的范围，SST 属于哪个 Level，等等。

当我们打开一个已经存在的数据库目录时，LevelDB 怎么知道上次关闭数据库时的状态呢？各个 SST 文件都属于哪个 Level？

通过读取 MANIFEST 文件，LevelDB 就可以知道上次关闭数据库时的状态，将数据库恢复到上次关闭时的状态。

## CURRENT

CURRENT 文件中存储的是当前正在使用的 MANIFEST 文件。

当创建新的 MANIFEST 文件时，LevelDB 会先更新 CURRENT文件，使其指向新的 MANIFEST 文件，然后再将旧的 MANIFEST 文件删除。

倘若没有 CURRENT 文件，新 MANIFEST 文件创建后，还没来得及删除旧的 MANIFEST 文件，系统就崩溃了，那么 LevelDB 恢复时就不知道当前正在使用的 MANIFEST 文件是哪个，也就无法恢复到正确的状态。

## LOCK

LevelDB 只允许一个进程访问数据库，为了实现这个功能，LevelDB 会在数据库目录下创建一个 LOCK 文件，当进程访问数据库时，会先尝试获取这个文件的锁，如果获取成功，说明当前没有进程访问数据库，可以继续访问；如果获取失败，说明当前已经有进程在访问数据库，就不能再访问了。

## LOG

LOG 文件用于记录数据库的运行状态和一些重要的事件。以下是一些可能出现在LOG文件中的信息：

- 数据库的打开和关闭：当数据库被打开或关闭时，会在LOG文件中记录这个事件。

- 数据写入：当数据被写入数据库时，会在LOG文件中记录这个事件，包括写入的键值对的信息。

- 数据删除：当数据被从数据库中删除时，会在LOG文件中记录这个事件，包括被删除的键的信息。

- 错误和异常：如果在运行过程中发生错误或异常，会在LOG文件中记录这个事件，包括错误或异常的详细信息。

- 压缩操作：当进行压缩操作时，会在LOG文件中记录这个事件，包括压缩的级别和涉及的文件。

- MemTable和SST文件的创建和删除：当创建或删除MemTable和SST文件时，会在LOG文件中记录这个事件。