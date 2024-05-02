# TODO:
25. 会有很长的时间段，采样不到任何东西？ratio 0 others 0 dram 0 pm 0 应该是系统负载高了的原因或者采样频率小了。而热度阈值和采样频繁程度也是挂钩的
26. 还要处理系统负载高时，采样开销和准确度的影响（这是相互的，不应该让步）

# Problem：
再次启动时内核崩溃, 这问题时有时无(可能是清空缓存命令造成的echo 3 > /proc/sys/vm/drop_caches)。

# 获取状态
next_promotion_node，升级去的节点，找到对应DRAM节点；
next_demotion_node，降级去的节点，找到对应PM节点