
在多线程编程中, 有两个需要注意的问题, 一个是**数据竞争**, 另一个是**内存执行顺序**. 

## 什么是数据竞争(Data Racing)

我们先来看什么是**数据竞争(Data Racing)**, 数据竞争会导致什么问题.

```c++
#include <iostream>
#include <thread>

int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i) {
        // ++counter实际上3条指令
        // 1. int tmp = counter;
        // 2. tmp = tmp + 1;
        // 3. counter = tmp;
        ++counter;
    }
}

int main() {
    std::thread t1(increment);
    std::thread t2(increment);

    t1.join();
    t2.join();

    std::cout << "Counter = " << counter << "\n";

    return 0;
}
```

在这个例子中, 我们有一个全局变量`counter`, 两个线程同时对它进行增加操作. 理论上, `counter`的最终值应该是`200000`. 然而, 由于数据竞争的存在, counter的实际值可能会小于`200000`. 

这是因为, `++counter`并不是一个原子操作, CPU会将`++counter`分成3条指令来执行, 先读取counter的值, 增加它, 然后再将新值写回`counter`. 示意图如下:

```plaintext
Thread 1                  Thread 2
---------                 ---------
int tmp = counter;        int tmp = counter;
tmp = tmp + 1;            tmp = tmp + 1;
counter = tmp;            counter = tmp;
```

两个线程可能会读取到相同的`counter`值, 然后都将它增加`1`, 然后将新值写回`counter`. 这样, 两个线程实际上只完成了一次`+`操作, 这就是数据竞争. 

## 如何解决数据竞争

c++11引入了`std::atomic<T>`, 将某个变量声明为`std::atomic<T>`后, 通过`std::atomic<T>`的相关接口即可实现原子性的读写操作.

现在我们用`std::atomic<T>`来修改上面的`counter`例子, 以解决数据竞争的问题.

```c++
#include <iostream>
#include <thread>
#include <atomic>

// 只能用bruce-initialization的方式来初始化std::atomic<T>.
// std::atomic<int> counter{0} is ok, 
// std::atomic<int> counter(0) is NOT ok.
std::atomic<int> counter{0}; 

void increment() {
    for (int i = 0; i < 100000; ++i) {
        ++counter;
    }
}

int main() {
    std::thread t1(increment);
    std::thread t2(increment);

    t1.join();
    t2.join();

    std::cout << "Counter = " << counter << "\n";

    return 0;
}
```

我们将`int counter`改成了`std::atomic<int> counter`, 使用`std::atomic<int>`的`++`操作符来实现原子性的自增操作.

`std::atomic`提供了以下几个常用接口来实现原子性的读写操作, `memory_order`用于指定内存顺序, 我们稍后再讲.

```c++
// 原子性的读取值
std::atomic<T>::store(T val, memory_order sync = memory_order_seq_cst);

// 原子性的写入值
std::atomic<T>::load(memory_order sync = memory_order_seq_cst);

// 原子性的增加
// counter.fetch_add(1)等价于++counter
std::atomic<T>::fetch_add(T val, memory_order sync = memory_order_seq_cst);

// 原子性的减少
// counter.fetch_sub(1)等价于--counter
std::atomic<T>::fetch_sub(T val, memory_order sync = memory_order_seq_cst);

// 原子性的按位与
// counter.fetch_and(1)等价于counter &= 1
std::atomic<T>::fetch_and(T val, memory_order sync = memory_order_seq_cst);

// 原子性的按位或
// counter.fetch_or(1)等价于counter |= 1
std::atomic<T>::fetch_or(T val, memory_order sync = memory_order_seq_cst);

// 原子性的按位异或
// counter.fetch_xor(1)等价于counter ^= 1
std::atomic<T>::fetch_xor(T val, memory_order sync = memory_order_seq_cst);
```

## 什么是内存顺序(Memory Order)

内存顺序是指在并发编程中, 对内存读写操作的执行顺序. 这个顺序可以被编译器和处理器进行优化, 可能会与代码中的顺序不同, 这被称为指令重排. 

我们先举例说明什么是指令重排, 假设有以下代码:

```c++
int a = 0, b = 0, c = 0, d = 0;

void func() {
    a = b + 1;
    c = d + 1;
}
```

在这个例子中, a = b + 1和c = d + 1这两条语句是独立的, 它们没有数据依赖关系. 如果处理器按照代码的顺序执行这两条语句, 那么它必须等待a = b + 1执行完毕后, 才能开始执行c = d + 1. 

但是, 如果处理器可以重排这两条语句的执行顺序, 那么它就可以同时执行这两条语句, 从而提高程序的执行效率. 例如, 处理器有两个执行单元, 那么它就可以将a = b + 1分配给第一个执行单元, 将c = d + 1分配给第二个执行单元, 这样就可以同时执行这两条语句. 

这就是指令重排的好处：它可以充分利用处理器的执行流水线, 提高程序的执行效率. 

但是在多线程的场景下, 指令重排可能会引起一些问题, 我们看个下面的例子:

```cpp
#include <iostream>
#include <thread>
#include <atomic>

std::atomic<bool> ready{false};
std::atomic<int> data{0};

void producer() {
    data.store(42, std::memory_order_relaxed); // 原子性的更新data的值, 但是不保证内存顺序
    ready.store(true, std::memory_order_relaxed); // 原子性的更新ready的值, 但是不保证内存顺序
}

void consumer() {
    // 原子性的读取ready的值, 但是不保证内存顺序
    while (!ready.load(memory_order_relaxed)) {   
        std::this_thread::yield(); // 啥也不做, 只是让出CPU时间片
    }

    // 当ready为true时, 再原子性的读取data的值
    std::cout << data.load(memory_order_relaxed);  // 4. 消费者线程使用数据
}

int main() {
    // launch一个生产者线程
    std::thread t1(producer); 
    // launch一个消费者线程
    std::thread t2(consumer);
    t1.join();
    t2.join();
    return 0;
}
```

在这个例子中, 我们有两个线程：一个生产者（producer）和一个消费者（consumer）. 

生产者先将`data`的值修改为一个有效值, 比如`42`, 然后再将`ready`的值设置为`true`, 通知消费者可以读取`data`的值了. 

我们预期的效果应该是当`消费者`看到`ready`为`true`时, 此时再去读取`data`的值, 应该是个有效值了, 对于本例来说即为`42`.

但实际的情况是, 消费者看到`ready`为`true`后, 读取到的`data`值可能仍然是`0`.  为什么呢? 

一方面可能是`指令重排`引起的. 在`producer`线程里, `data`和`store`是两个不相干的变量, 所以编译器或者处理器可能会将`data.store(42, std::memory_order_relaxed);`重排到`ready.store(true, std::memory_order_relaxed);`之后执行, 这样`consumer`线程就会先读取到`ready`为`true`, 但是`data`仍然是`0`.

另一方面可能是`内存顺序`不一致引起的. 即使`producer`线程中的指令没有被重排, 但`CPU的多级缓存`会导致`consumer`线程看到的`data`值仍然是`0`. 我们通过下面这张示意图来说明这和CPU多级缓存有毛关系.

![CPU多级缓存](https://img-blog.csdnimg.cn/img_convert/1f7b8443bdab77a1da8bc30eb407b79c.png)

每个CPU核心都有自己的`L1 Cache`与`L2 Cache`. `producer`线程修改了`data`和`ready`的值, 但修改的是`L1 Cache`中的值, `producer`线程和`consumer`线程的`L1 Cache`并不是共享的, 所以`consumer`线程不一定能及时的看到`producer`线程修改的值. `CPU Cache`的同步是件很复杂的事情, `生产者`更新了`data`和`ready`后, 还需要根据`MESI协议`将值写回内存,并且同步更新其他CPU核心`Cache`里`data`和`ready`的值, 这样才能确保每个CPU核心看到的`data`和`ready`的值是一致的. 而`data`和`ready`同步到其他`CPU Cache`的顺序也是不固定的, 可能先同步`ready`, 再同步`data`, 这样的话`consumer`线程就会先看到`ready`为`true`, 但`data`还没来得及同步, 所以看到的仍然是`0`.

这就是我们所说的`内存顺序`不一致问题.

为了避免这个问题, 我们需要在`producer`线程中, 在`data`和`ready`的更新操作之间插入一个`内存屏障(Memory Barrier)`, 保证`data`和`ready`的更新操作不会被重排, 并且保证`data`的更新操作先于`ready`的更新操作. 

现在我们来修改上述的例子, 来解决`内存顺序`不一致的问题.

```cpp
#include <iostream>
#include <thread>
#include <atomic>

std::atomic<bool> ready{false};
std::atomic<int> data{0};

void producer() {
    data.store(42, std::memory_order_relaxed); // 原子性的更新data的值, 但是不保证内存顺序
    ready.store(true, std::memory_order_released); // 保证data的更新操作先于ready的更新操作
}

void consumer() {
    // 保证先读取ready的值, 再读取data的值
    while (!ready.load(memory_order_acquire)) {   
        std::this_thread::yield(); // 啥也不做, 只是让出CPU时间片
    }

    // 当ready为true时, 再原子性的读取data的值
    std::cout << data.load(memory_order_relaxed);  // 4. 消费者线程使用数据
}

int main() {
    // launch一个生产者线程
    std::thread t1(producer); 
    // launch一个消费者线程
    std::thread t2(consumer);
    t1.join();
    t2.join();
    return 0;
}
```

我们只是做了两处改动, 

- 将`producer`线程里的`ready.store(true, std::memory_order_relaxed);`改为`ready.store(true, std::memory_order_released);`, 一方面限制`ready`之前的所有操作不得重排到`ready`之后, 以保证先完成`data`的写操作, 再完成`ready`的写操作.  另一方面保证先完成`data`的内存同步, 再完成`ready`的内存同步, 以保证`consumer`线程看到`ready`新值的时候, 一定也能看到`data`的新值.

- 将`consumer`线程里的`while (!ready.load(memory_order_relaxed))`改为`while (!ready.load(memory_order_acquire))`, 限制`ready`之后的所有操作不得重排到`ready`之前, 以保证先完成读`ready`操作, 再完成`data`的读操作;

现在我们再来看看`memory_order`的所有取值与作用: 

- `memory_order_relaxed`: 只确保操作是原子性的, 不对`内存顺序`做任何保证, 会带来上述`produer-consumer`例子中的问题.

- `memory_order_release`: 用于写操作, 比如`std::atomic::store(T, memory_order_release)`, 会在写操作之前插入一个`StoreStore`屏障, 确保屏障之前的所有操作不会重排到屏障之后, 如下图所示

```plaintext
      +
      |
      |
      | No Moving Down
      |
+-----v---------------------+
|     StoreStore Barrier    |
+---------------------------+
```

- `memory_order_acquire`: 用于读操作, 比如`std::atomic::load(memory_order_acquire)`, 会在读操作之后插入一个`LoadLoad`屏障, 确保屏障之后的所有操作不会重排到屏障之前, 如下图所示:

```plaintext
+---------------------------+
|     LoadLoad Barrier      |
+-----^---------------------+
      |
      |
      | No Moving Up
      |
      |
      +
```

- `memory_order_acq_rel`: 等效于`memory_order_acquire`和`memory_order_release`的组合, 同时插入一个`StoreStore`屏障与`LoadLoad`屏障. 用于读写操作, 比如`std::atomic::fetch_add(T, memory_order_acq_rel)`. 

- `memory_order_seq_cst`: 最严格的内存顺序, 在`memory_order_acq_rel`的基础上, 保证所有线程看到的内存操作顺序是一致的. 这个可能不太好理解, 我们看个例子([该例子引自这](https://www.enseignement.polytechnique.fr/informatique/INF478/docs/Cpp/en/cpp/atomic/memory_order.html#:~:text=Sequentially%2Dconsistent%20ordering)), 什么情况下需要用到`memory_order_seq_cst`:

```cpp
#include <thread>
#include <atomic>
#include <cassert>
 
std::atomic<bool> x = {false};
std::atomic<bool> y = {false};
std::atomic<int> z = {0};
 
void write_x()
{
    x.store(true, std::memory_order_seq_cst);
}
 
void write_y()
{
    y.store(true, std::memory_order_seq_cst);
}
 
void read_x_then_y()
{
    while (!x.load(std::memory_order_seq_cst))
        ;
    if (y.load(std::memory_order_seq_cst)) {
        ++z;
    }
}
 
void read_y_then_x()
{
    while (!y.load(std::memory_order_seq_cst))
        ;
    if (x.load(std::memory_order_seq_cst)) {
        ++z;
    }
}
 
int main()
{
    std::thread a(write_x);
    std::thread b(write_y);
    std::thread c(read_x_then_y);
    std::thread d(read_y_then_x);
    a.join(); b.join(); c.join(); d.join();
    assert(z.load() != 0);  // will never happen
}
```

以上这个例子中, `a`, `c`, `c`, `d`四个线程运行完毕后, 我们期望`z`的值一定是不等于0的, 也就是`read_x_then_y`和`read_y_then_x`两个线程至少有一个会执行`++z`. 要保证这个期望成立, 我们必须对`x`和`y`的读写操作都使用`memory_order_seq_cst`, 以保证所有线程看到的内存操作顺序是一致的. 

什么意思呢, 如果不使用`memory_order_seq_cst`, `read_x_then_y`可能会先看到`y`为`true`才看到`x`为`true`, 同时`read_y_then_x`也可能会先看到`x`才看到`y`, 因为`write_x`和`write_y`是在不同的CPU核心上执行, `read_x_then_y`有可能先同步`y`再同步`x`, `read_y_then_x`也有可能先同步`x`再同步`y`, 这样就会导致`read_x_then_y`与`read_y_then_x`都无限阻塞在`while`循环里.

使用`memory_order_seq_cst`标记的读写操作将会以一致的内存顺序执行, 比如`read_x_then_y`的CPU核心如果先同步`y`再同步`x`, 那么`read_y_then_x`的CPU核心也一定会先同步`y`再同步`x`, 这样就能保证`read_x_then_y`与`read_y_then_x`总有一个能执行到`++z`.

这样说可能还是会比较抽象, 我们可以直接理解下`memory_order_seq_cst`是怎么实现的. 被`memory_order_seq_cst`标记的写操作, 会立马将新值写回内存, 而不仅仅只是写到`Cache`里就结束了; 被`memory_order_seq_cst`标记的读操作, 会立马从内存中读取新值, 而不是直接从`Cache`里读取. 这样相当于`write_x`, `write_y`, `read_x_then_y`, `read_y_then_x`四个线程都是在同一个内存中读写`x`和`y`, 也就不存在`Cache同步`的顺序不一致问题了. 

所以我们也就能理解为什么`memory_order_seq_cst`会带来最大的性能开销了, 相比其他的`memory_order`来说, 因为相当于禁用了CPU `Cache`.

`memory_order_seq_cst`常用于`multi producer - multi consumer`的场景, 比如上述例子里的`write_x`和`write_y`都是`producer`, 会同时修改`x`和`y`, `read_x_then_y`和`read_y_then_x`两个线程, 都是`consumer`, 会同时读取`x`和`y`, 并且这两个`consumer`需要按照相同的内存顺序来同步`x`和`y`.