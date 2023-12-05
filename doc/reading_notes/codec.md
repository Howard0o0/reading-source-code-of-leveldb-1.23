# LevelDB的序列化: VarInt与FixedInt

我们都知道大名鼎鼎的`ProtoBuf`与`FlatBuffers`，它们都是序列化工具，用于将数据序列化为二进制数据，以便于存储与传输。

`LevelDB`同样也有序列化的需求，以便于将内存中的数据存储到磁盘里。但是`LevelDB`中用不到`ProtoBuf`与`FlatBuffers`这样的工具，因为`LevelDB`的序列化对象只有`Int`这一种类型，所以`LevelDB`的序列化与反序列化是自己实现的，可以将`Int`序列化为`VarInt`或者`FixedInt`。

## FixedInt

`FixedInt`就是将`int32_t`编码为一个4字节的序列，将`int64_t`编码为一个8字节的序列。这种编码方式的优点是编码后的序列长度固定，方便读取，缺点是对于较小的数值，编码后的序列长度比实际需要的要大，造成空间浪费。

举例说明对于一个 32 位整数（如 `int32_t`），LevelDB 将其编码为一个 4 字节的序列。

假设有一个 `int32_t` 类型的整数 `value = 0x12345678`，在内存中的表示（假设是小端字节序的机器）为 `78 56 34 12`。编码过程如下：

1. 取出 `value` 的最低有效字节（LSB），即 `0x78`。
2. 接着是次低字节，`0x56`。
3. 然后是次高字节，`0x34`。
4. 最后是最高有效字节（MSB），`0x12`。

编码后的字节序列为 `78 56 34 12`。

`int64_t` 类型的整数编码过程与 `int32_t` 一样，只是编码长度多了 4 个字节而已。

这样讲可能还是比较抽象，talk is cheap，let's see the code.

### FixedInt32

#### Encode

```cpp
// 将value编码成4字节，并写入dst中
inline void EncodeFixed32(char* dst, uint32_t value) {

    // 将char*转成uint8_t*，避免溢出
    uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

    // 按照Little-Endian的方式将value写入buffer中
    buffer[0] = static_cast<uint8_t>(value);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value >> 16);
    buffer[3] = static_cast<uint8_t>(value >> 24);
}
```

可能同学会有问题，为什么要把`char*`转成`uint8_t*`呢？

因为表示范围不一样，有的平台`char`是`signed char`，有的平台`char`是`unsigned char`。如果是`signed char`，那么`char`的范围是`-128 ~ 127`，而`uint8_t`的范围是`0 ~ 255`，如果将一个值大于`127`的`uint8_t`赋值给`char`，那么`char`的值就会溢出，我们可以通过下面的代码来验证：

```cpp
char a = 200;
if (a == 200) {
    printf("yes"); 
} else {
    printf("no"); // amd64平台下，输出no
}
```

还有的同学可能对c++中的类型转还不太了解，为什么这里要用`reinterpret_cast`而不用其他的，比如`static_cast`呢？

关于C++的类型转换，移步[大话C++之：类型转换](https://blog.csdn.net/sinat_38293503/article/details/134710045?spm=1001.2014.3001.5502)，5分钟讲明白😁。


#### Decode

没啥好说的，能理解`Encode`就能理解`Decode`。

```c++
inline uint32_t DecodeFixed32(const char* ptr) {
    const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

    // Recent clang and gcc optimize this to a single mov / ldr instruction.
    return (static_cast<uint32_t>(buffer[0])) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);
}
```

### FixedInt64

`FixedInt64`与`FixedInt32`的实现基本一致，编解码的方式都是一样的，唯一的区别是编码长度不一样。

#### Encode

```c++
inline void EncodeFixed64(char* dst, uint64_t value) {
    uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

    // Recent clang and gcc optimize this to a single mov / str instruction.
    buffer[0] = static_cast<uint8_t>(value);
    buffer[1] = static_cast<uint8_t>(value >> 8);
    buffer[2] = static_cast<uint8_t>(value >> 16);
    buffer[3] = static_cast<uint8_t>(value >> 24);
    buffer[4] = static_cast<uint8_t>(value >> 32);
    buffer[5] = static_cast<uint8_t>(value >> 40);
    buffer[6] = static_cast<uint8_t>(value >> 48);
    buffer[7] = static_cast<uint8_t>(value >> 56);
}
```

#### Decode

```c++
inline uint64_t DecodeFixed64(const char* ptr) {
    const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

    // Recent clang and gcc optimize this to a single mov / ldr instruction.
    return (static_cast<uint64_t>(buffer[0])) |
           (static_cast<uint64_t>(buffer[1]) << 8) |
           (static_cast<uint64_t>(buffer[2]) << 16) |
           (static_cast<uint64_t>(buffer[3]) << 24) |
           (static_cast<uint64_t>(buffer[4]) << 32) |
           (static_cast<uint64_t>(buffer[5]) << 40) |
           (static_cast<uint64_t>(buffer[6]) << 48) |
           (static_cast<uint64_t>(buffer[7]) << 56);
}
```

## Varint

和`FixedInt`不同，`Varint`是一种变长编码方式，它的编码长度是不固定的，对于较小的数值，编码后的序列长度比实际需要的要小，节省空间。

比如对于一个`int64_t`类型的整数，值为`1`。如果使用`FixedInt`编码，那么编码后的序列长度为**8**字节，而使用`Varint`编码，编码后的序列长度仅为**1**字节。

以数字`300`为例，进行`Varint`编码的步骤说明。

**1. 转换为二进制**

十进制 `300` 在二进制中表示为 `100101100`.

**2. 划分为7位一组**

从低位开始，每**7**位分为一组：`0010110` 和 `0000100`.

**3. 添加控制位**

每组前添加**1bit**的控制位，`1` 表示后面还有数据，`0` 表示这是最后一个字节。

- `0010110` -> `10010110` （因为后面还有字节，所以在前面加 `1`）
- `0000100` -> `00000100` （这是最后一个字节，所以在前面加 `0`）

**4. Varint编码结果**

- `10010110 00000100`

在这个例子中，数字 `300` 被编码为两个字节：`10010110 00000100`。这种编码方法对于 LevelDB 来说非常重要，因为它可以节省存储空间并提高处理效率。

### Encode

可以将`uint32_t`或者`uint64_t`编码为`Varint`。

`EncodeVarint32`:

```c++
/* 将 uint32_t 编码成 Varint32 并写入至 dst 指向的地址中，同时返回新的指针地址 */
char* EncodeVarint32(char* dst, uint32_t v) {
    // Operate on characters as unsigneds
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
    static const int B = 128;
    if (v < (1 << 7)) {
        *(ptr++) = v;
    } else if (v < (1 << 14)) {
        *(ptr++) = v | B;
        *(ptr++) = v >> 7;
    } else if (v < (1 << 21)) {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = v >> 14;
    } else if (v < (1 << 28)) {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = (v >> 14) | B;
        *(ptr++) = v >> 21;
    } else {
        *(ptr++) = v | B;
        *(ptr++) = (v >> 7) | B;
        *(ptr++) = (v >> 14) | B;
        *(ptr++) = (v >> 21) | B;
        *(ptr++) = v >> 28;
    }
    return reinterpret_cast<char*>(ptr);
}
```

`EncodeVarint64`:

```c++
char* EncodeVarint64(char* dst, uint64_t v) {
    static const int B = 128;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
    while (v >= B) {
        *(ptr++) = v | B;
        v >>= 7;
    }
    *(ptr++) = static_cast<uint8_t>(v);
    return reinterpret_cast<char*>(ptr);
}
```

这里可能会有小伙伴问了，明明编码方式是一样的，为什么`EncodeVarint32`不使用`for循环`而`EncodeVarint64`使用`for循环`呢？

`EncodeVarint32`处理的是32位的整数，最多只需要5个字节就可以表示。因此，它可以通过分组复制的方式，直接将整数的每个部分编码到结果中。这种方式的代码比较**直观**，易于理解。

而`EncodeVarint64`处理的是64位的整数，最多需要10个字节才能表示。如果还是采用分组复制的方式，那么代码将会变得非常冗长。这种方式的代码简洁，但不如`EncodeVarint32`的写法直白。

所以`EncodeVarint32`不使用`for循环`只是为了增强代码的可读性，没有效率上的差别。

### Decode

`p`是`Varint32`的起始地址，`limit`是`Varint32`的结束地址。

```c++

```c++
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p));
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}
```

当要解码的数字小于等于127时，直接返回结果。(这个设计是基于大部分数字都比较小于127，并且这个函数还是内联函数来提高效率)。

当待解码的数字大于127时，调用`GetVarint32PtrFallback`。

```c++
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}
```

`Varint64`的解码同理。

```c++
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
        p++;
        if (byte & 128) {
            // More bytes are present
            result |= ((byte & 127) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return reinterpret_cast<const char*>(p);
        }
    }
    return nullptr;
}
```


## FixedInt 与 Varint 的适用场景

`FixedInt`的编解码速度快，但是会浪费空间，属于空间换时间的做法。

`Varint`的编解码速度慢，但是节省空间，属于时间换空间的做法。

- 被频繁调用，并且出现值较大数的概率偏大时，选择`FixedInt`。
- 当数据量较大，且经常出现较小数值时，选择`Varint`可以节省存储空间，但不会损失多少性能。