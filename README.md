# TODO:
9. 页面被锁住无法向上迁移。
10. active lru几乎都为null.


# Problem：
降温和迁移前后需要调整LRU链表，对LRU的调整具体？考虑大页和小页？应该是直接找到头尾move过去的。从火焰图来看没有明显开销。但是要注意，没被采样的该怎么麽办，会不会造成不公平？
页面拆分不能有了，因为没了直方图我没法判断了。


# 获取状态

## 1.1 浮点数的状态
浮点性能参考指标 (xFLOPS) = 总运算核心数 x 每周期运算次数 x 处理器相对运作频率（ 384 Core x 4 x 800 MHz(0.8 GHz)）
所有 60 个内核都可以以 16 次/时钟的速率执行双精度浮点数学。
以下是英特尔至强融核 5110 的峰值理论浮点数学：
GFLOPS = 60 cores/Phi * 1.053 GHz/core * 16 GFLOPs/GHz=1,010.8 = 一个CPU核的数量*每秒多少时钟周期*每周期浮点数运算次数

### （1）获取cpu当前的实时频率
在系统进行cpu性能测试主要是linpack测试的过程中执行该脚本，该脚本会自动获取当前的cpu频率；
F=`cat/proc/cpuinfo|grep-iMHz|awk'{print$4}'|head-n1`
### （2）脚本自动获取系统下cpu的总核心数
N=`cat/proc/cpuinfo|grep-iprocessor|wc–l`
### （3）获取当前cpu每周期的浮点运算次数并计算理论值
```
cat/proc/cpuinfo|grep-iavx2
if[$?-eq0];then
Flops=`echo“$F*$N*16”|bc`
else
Flops=`echo“$F*$N*8”|bc`
fi
```
### （4）显示CPU型号，系统下的核心数，当前频率和浮点运算理论值
```
cat/proc/cpuinfo|grep“modelname”|head–n1
echo“CPUFreqis$F”
echo“CPUCoresare$N”
echo“Flopsis$FlopsMFlops”。
```

## 1.2内存带宽的状态
### 22FAST 
MT平方

### 脚本自动获取内存channel总数和内存当前频率
手动输入单cpu下内存channel数，脚本会读取该数据为之后计算做准备。
A.脚本会自动获取当前系统的cpu总数：
cpu_num=`cat/proc/cpuinfo|grep-i"physicalid"|awk'{print$4}'|tail-1`
B.然后获取内存的当前频率：
speed=`dmidecode-tmemory|grep-i'configuredclockspeed'|awk'{print$4}'|head-1`
最后根据这些信息计算出内存的理论带宽值。

内存带宽的计算公式是：带宽=内存核心频率×内存总线位数×倍增系数。简化公式为：标称频率*位数。比如一条DDR3 1333MHz 64bit的内存，理论带宽为：1333*64/8=10664MiB/s = 10.6GiB/s。

例如DDR266的频率即为266MHz,根据内存带宽的算法:带宽=总线位宽/8x1个时钟周期内交换的数据包个数*总线频率，DDR266的带宽= 64/8x2x133=2128,它的传输带宽为2.1GB/s，因此DDR266又俗称为PC2100，这里的2100就是指其内存带宽约为2100MB。https://blog.51cto.com/zhangheng/941012

### perf
sudo perf stat -a -e uncore_imc/data_reads/

### mbw工具
ubuntu下已经可以直接安装使用
无须下载源码编译https://www.cnblogs.com/sunshine-blog/p/11903842.html。安装命令：apt-get install mbw
常用命令：mbw -q -n 10 256
-n 10表示运行10次，256表示测试所用的内存大小，单位为MB。
mbw测试了MEMCPY、DUMB、MCBLOCK等方式的内存带宽。从测试结果看，前2都差不多，最后一种测试得到的带宽值比较高

### python算内存带宽
https://www.volcengine.com/theme/7266637-B-7-1


Linux kernel
============

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.