## early-dispatch机制设计思考

### 术语简写

- p core：predecessor core
- c core：successor core
- p task：predecessor task
- c task：successor task
- ed：early-dispatch

### 设计初衷

- 现状：AI core -〉AI CPU -〉处理依赖、入ready queue -〉AI CORE
  - successor core任务需要在predecessor core任务完成后，由schduler接受Fin信号，解依赖后完成task下发至successor core，引入额外调度开销；
- 目标：AI core -〉AI CORE（check 依赖并执行）
  - 在predecessor core执行期间，事先选定AI Core下发successor任务，predecessor core任务完成直接发送Fin信号至successor core，直接触发执行；

### 已有基础及不足

- simpler仓库：AI core -〉AI CPU -〉处理依赖-〉AI CORE
  - 在predecessor core执行期间，事先选定**空闲AI Core**下发successor任务，predecessor core任务完成发送Fin信号至AI CPU，AI CPU解依赖后dooebell到successor core，触发执行；
- 问题：
  1. 选定**空闲AI Core**导致early-dispatch范围有限——最多再选p task节点，没办法发到其他节点，因为不知道其他节点core的任务啥时候完成；
  2. 选定**空闲AI Core**导致空闲core需要循环空转？（比如该core空转期间，有任务ready了需要派发怎么办，能抢占吗——应该可以抢占，core轮询outstanding，哪个ready了就上哪个，但如果outstanding被ed塞满了，那确实没办法）
  3. 调度路径长度仍然较长AI core -〉AI CPU -〉处理依赖-〉AI CORE；



### 设计构思与预期收益

- 设计构思
  1. early-dispatch的对象优先选为predecessor core，可应付1对1，多对1场景；#predecessor core < #successor任务（1对多场景），**可选择空闲core派发？**
  2. predecessor core运行结束，采用单播或广播，通过HSCB直发FIN信号到successor core，触发执行；
- 预期收益
  1. 调度路径缩短为AI core -〉AI CORE；
  2. early-dispatch到predecessor core可尽量避免core空转；



### 关键方案设计



#### 具体方案（以DV121为假设场景设计）

- DV121硬件约束
  1. DV121当前仅支持单播，不支持广播操作，即使是CPU派发也是单播；
  2. DV121支持节点的Loop环回发信号；
- 方案1（放弃，收益受到串链影响）: AI core -〉AI CORE
  - 思路：每个predecessor任务下发时就带上early-dispatch的successor core的地址hint；
  - 好处：predecessor core任务完成时根据地址hint直接发送Fin信号到对应successor core；
  - 问题：对于有依赖的DAG图，该方案类似串链表，且链头依赖AI CPU触发，因此收益正比于：（链长-1）*（每节链的successor分叉数），则意味着每次early-dispatch时，要尽可能选择outstanding空闲多、串链尽可能长的时候进行，导致：
    1. early-dispatch要感知outstanding数量，灵活性受限；——AI CPU可以感知core的outstanding情况；
    2. early-dispatch要尽可能将可用的outstanding占满以增加链长，导致高优先级临时任务难以抢占？
    3. 链长受限于AI core的outstanding规格影响，收益下滑；
    4. dispatch链头时，要一口气尽可能长的early-dispatch后续链节点，如果链节点位置需要动态决定，则必须中断，导致灵活性受限；
- 方案2（待定，需要原子操作）: AI core -〉AI CORE
  - 思路：每early-dispatch的successor 任务时，将successor core的地址hint发送到已派发的predecessor core上面；
  - 好处：predecessor core任务完成时根据地址hint直接发送Fin信号到对应successor core；
  - 问题：
    1. 朝正在运行的predecessor core刷新hint数据，需要复杂原子操作？
    2. 发送hint数据时，predecessor core刚好已经通知已记录的successor core，该如何处理（AI CPU发）
    3. 未记录的successor core由AI CPU发Fin信号，但AI CPU怎么除去predecessor core已记录的hint？（每发一个hint就ack一下？岂不是浪费带宽 --> **AICPU重复发，简单且易于实现**）
    4. predecessor core的bitmap是否容易获得？
- 方案3（演进到方案4）:AI core -> Fin地址 + AI core -> Fin地址 -> AI core
  - 思路：约定好p core的Fin写入位置，在ed 新的是s core时，将Fin位置作为hint传入s core；当s core完成当前任务后，采用pull的形式拉去Fin信号触发新任务执行
  - 好处：无需方案2中ed时动态下发s core地址hint到p core；
  - 问题：
    1. 调度路径变为AI core -> Fin地址 + AI core -> Fin地址 -> AI core，相比AI core -> AI core多了一个RTT；则：
      - p core约定的Fin地址在HBM还是临近cache？如果临近，则引入RTT代价不高；
      - 这种情况下，似乎还不如就AI core -> AI CPU -> AI core路径更短；
    2. 在1对多场景下，多个s core需要读Fin信号，那么Fin信号需要采用cnt计数来判断何时释放；
    3. 在多对1场景下，一个s core需要读多个Fin信号，那么s core需要采用Bitmap来判断依赖Fin是否收齐，简单cnt够吗？是否会和AI CPU来的Fin信号重复计数？
- 方案4.1(AI CPU中转Fin信号，类似simpler仓现有的ed，但是少了AI CPU解依赖步骤): AI core-〉AI CPU-〉AI core
  - 思路：p core执行完成之后，返回Fin信号给AI CPU，由AI CPU中转Fin信号到目的s core，触发执行；
  - 好处：无需hint，无需原子操作，尽可能复用现有机制；
  - 问题：
    1. 调度路径变为AI core-〉AI CPU-〉AI core，AI CPU可能中转较慢，引起任务等待造成Core计算空闲；
- 方案4.2(STARS中转Fin同步，即MPMC):AI core-〉STARS-〉AI core
  - 思路：AI CPU ed任务到s core的时候，由AI CPU注册wait到STARS，而后p core运行完之后发送Fin信号到STARS，STARS完成向s core的Fin信号同步；（STARS信号直接通知到AI Core的SPR上，AI Core空闲时polling）
  - 好处：STARS中转较AI CPU更快；
  - 问题：
    1. p core怎么知道Fin信号是否要发到STARS？
      - 选择都发：浪费带宽；
      - STARS收到wait注册后，通知p core要发：交付复杂点；且可能在STARS通知p core时，p core刚好结束，这个可以由AI CPU的发送兜底；



#### 遗留问题

1. 方案问题澄清——已澄清；
2. DV121上的stars和core上的有个early start和fake end的功能可以利用？——这个是core上的乒乓buffer的执行设计，与调度暂时判断无关，暂不考虑；



#### 代码落地

- 整体开发策略：
  1. 优先开发AI CPU中转方案；---方案4.1--王帅
  2. 其次开发STARS中转方案，STARS交互接口代码找——谢洁、思扬；---方案4.2
  3. 针对1to1的任务依赖，可考虑采用方案2优化；---方案2--张程博
- 代码开发关键点（方案4.1，20260723）：     
  1. sch新增early-dispatch机制:
    - early-dispatch条件：s task的前驱p task均已下发，且un-Fin的p task数量< N(N可调，暂定为1)；
    - s core筛选：优选p core节点，次选空闲AI core；
  2. AI core kernel新增解依赖机制：
    - 根据记录的un_Fin_predecessor对收到的Fin信号进行依赖解除判断；

