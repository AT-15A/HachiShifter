# ex → 统一原生版本：UTAU 功能审计附表

状态：静态审计；未执行 ex，也未改业务源码。H 表示当前 `juce/src`，E 表示 `../ex/juce/src`。行号为审计时版本，后续实施会变化。主方案见 `UTAU_UNIFIED_EDITOR_PLAN.md`。

## 1. 不能误报为缺失的现有能力

H 已有：经典/界/谋模式、时序与 STP、库级 OTO 波形分区编辑和保存、颤音参数/真实线/烘焙标点、flag 分区与重置、辅音重置、时序删除/插空格、批量歌词、音高编辑、音符分割/合并、四种包络预设、合成波形、UST 导入、UTAU 外部引擎、prefix.map 等。

证据：H `PianoRollComponent.cpp:1945–1973` 的菜单及 `:2502,2587,2792,2867`；H `OtoWaveformEditorComponent.cpp:595,921,1011,1045`；H `MainComponent.cpp` 包络/导入/编辑入口。当前又有 ex 没有的 NativeSegment/NativeConnection、HJM v2 与 MelodyneProvider 层，不能整文件替换。

## 2. ex 增量与迁移验收点

| ID | 缺失/增强项 | E 证据 | 迁入原生层的要求 |
| --- | --- | --- | --- |
| U01 | 单音符 OTO 与恢复库 OTO | PianoRollComponent.cpp:2265,2270,2413；MainComponent.cpp:1523；OtoWaveformEditorComponent.cpp:1200,1244 | 原生局部素材覆盖，HJPX存储；仅库级编辑写OTO |
| U02 | OTO原音试听、停止、播放头 | OtoWaveformEditorComponent.cpp:1261,1267,1295,1313；MainComponent.cpp:1545 | 复用音频设备；区分原音试听和渲染试听；不影响歌曲位置 |
| U03 | Tab/Shift+Tab歌词连续录入 | PianoRollComponent.cpp:5334,5388,8156 | 同轨跨clip顺序、焦点、自动滚动及一次编辑撤销 |
| U04 | 汉字转拼音 | MainComponent.cpp:3209,3288；Pinyin.cpp:20,32 | 按选择/轨道应用统一歌词命令，不要求UTAU来源；保留拉丁/前后缀 |
| U05 | 添加前置拼字音符 | PianoRollComponent.cpp:2246,2394,2835；ProjectModel.h:737–759 | 原生前置发音事件/零时值语义；不可与已有拼接按钮混淆 |
| U06 | 包络基础值0–200% | PianoRollComponent.cpp:2312,2898,2928 | 通用包络参数，隐含包络、多选、0静音、序列化 |
| U07 | 六种包络预设与幅度线性 | PianoRollComponent.cpp:461,507,537,576；backend/AmplitudeEnvelopeCurve.h | 保留旧工程插值；新增缓起/句尾，精确实现标准/柔起/渐弱/短收 |
| U08 | 包络波形背景和拖动实时塑形 | PianoRollComponent.cpp:4209,4232,4261,4280,4327,4423；AudioEngine.h:21–46 | 单音符真实片段、预发声对齐，包络与音色缓存分离 |
| U09 | 颤音终点独立编辑 | PianoRollComponent.cpp:3140,3179,7704；ProjectModel.h:290–293 | 终点为原创增强，不是UST七参数之一；HJPX保存，烘焙一致 |
| U10 | 共享音高线及有效点范围 | PianoRollComponent.cpp:3423,3459,3481,3492,7841；ProjectModel.h:424–498 | 接入NativeConnection/统一曲线求值，跨音符拖点也影响真实音频 |
| U11 | UTAU音符直接初始化音高线 | PianoRollComponent.cpp:2049,2133,2324 | 统一音高工具支持所有来源，初始化操作保留明确语义 |
| U12 | 扩展flags模式约束 | PianoRollComponent.cpp:1194,1207；MainComponent.cpp:1212；UtauRenderer.h:222 | 不照搬ex的编辑器禁用门控；统一编辑保留，渲染协议做能力检查 |
| U13 | 拼字附近辅音手柄/点击阈值 | PianoRollComponent.cpp:5186,7771,8094 | 点击无位移不能清除时序覆盖；重叠手柄可选择 |
| U14 | 音源后台索引 | PianoRollComponent.cpp:4144；MainComponent.cpp:747；UtauRenderer.h:175 | 迁至素材服务；UI不直接同步调用渲染器读全库 |
| U15 | UST替换工程/追加轨道及聚焦 | MainComponent.cpp:4335,4375,4390,4405；ProjectModel.cpp:1577–1733 | 新建/追加原生工程事务，Tempo策略明确，一次撤销 |
| U16 | MIDI单轨导入与导出 | MainComponent.cpp:1899,1949,1997,4283；ProjectModel.h:572–595 | 共有互通功能：歌词、拍号、Tempo和乐拍往返 |
| U17 | UST跨编码与VBR | backend/UstImporter.cpp:119–179,291–313 | 加编码选择兜底，不把启发式识别当100%准确；保留原参数 |
| U18 | UST音高不被额外自动转音改变 | ProjectModel.cpp:1629–1632；AudioEngine.cpp共享线/自动转音路径 | 导入转为明确连接策略，进入编辑后不按来源分支 |
| U19 | HF外部引擎daemon预热 | backend/UtauRenderer.h:155–169 | 按引擎能力和所配置解释器，异步、可诊断，不影响原生NSF后端 |
| U20 | 索引文件修订和别名哈希查询 | backend/UtauRenderer.cpp:433–648,705–727；SampleSettings.h:126–130 | OTO/prefix/扩展及素材提交失效；不能用整个文件夹mtime判断 |
| U21 | MY扩展曲线 | ProjectModel.cpp:158–185 | 扩展参数保存在原生工程，适配器检测引擎支持 |
| U22 | 波形/包络/拼字等测试入口 | Main.cpp、tools/regress.sh | 迁测试意图到统一模型；不可只复制旧测试并保留模式锁 |

已有和新增的分界必须逐项复核；其它 ex 改动，包括一般视图默认值，不应在没有必要时覆盖当前原生 UI 默认值。

## 3. 核心错误与副作用证据

| ID | 当前问题 | H证据 | 修复方向 |
| --- | --- | --- | --- |
| D01 | 任意HJM结果遮蔽整库OTO | backend/UtauRenderer.cpp:358–362 | 明确素材类型/绑定；不按目录存在性抢占 |
| D02 | UTAU注册写HJM | SampleSettings.cpp:1181–1188；AssetManagerComponent.cpp:31–58 | 默认只读登记，内存适配 |
| D03 | 经典OTO保存后二次写HJM | SampleSettings.cpp:987–1011 | 库级OTO事务直写，不隐式转HJM |
| D04 | cutoff负尾裁剪错误 | SampleSettings.cpp:775 对比 :724–728 | 正尾裁剪或负区间长度，数值往返 |
| D05 | 单音频OTO导出替换整文件 | SampleSettings.cpp:783 | 导出新建/合并/明确替换；库编辑使用行级事务 |
| D06 | nativeSegments导致歌词重绑定提前返回 | ProjectModel.cpp:2930–2956 对比 :1115 | 按素材身份变化重置局部覆盖，不以导入类型分支 |
| D07 | 工程歌词修改写标注文件 | ProjectModel.cpp:2882–2914,2960–2998 | 独立工程/素材命令和撤销边界 |
| D08 | 真实Melodyne能力未接通 | backend/MelodyneProvider.cpp:61–65,130–143；RenderService.cpp:675–684 | 独立提供者能力验收，不以ex自研MLD替代 |

## 4. 回归测试清单

ex新增的重要验证入口（仅确认源码存在，本轮未运行）：

- `--smoke-oto-playback`、`--smoke-note-oto`
- `--smoke-lyric-tab`、`--smoke-hanzi-pinyin`
- `--smoke-prefix-jie`、`--smoke-prefix-note`
- `--smoke-envelope-base`、`--smoke-envelope-shapes`、`--smoke-amplitude-waveform`
- `--smoke-vibrato-end`、`--smoke-shared-pitch-line`
- `--smoke-waveform-alignment`、`--smoke-note-waveform`
- `--smoke-voicebank-index`、`--smoke-hf-prewarm`
- `--smoke-ust-pitch`、`--smoke-ust-vibrato`、`--smoke-ust-import`、`--smoke-ust-encoding`
- `--smoke-midi-track-import`、`--smoke-midi-export`
- `--smoke-flag-curve-mode`（需改为原生编辑可用、引擎协议合法两层验收）

两边均有的 `utau-mode/utau-waveform/utau-overlap/utau-selection/utau-voicebank/utau` 等必须继续回归，不能当作新增测试。

还有 tests/ 下的 flags优先级、flag重渲染、区域flags、rest、splice、辅音保持、包络拉伸/尾部、mou计数和OTO隔离测试；以实际用例和引擎行为作为迁移验收矩阵。

实施时另加：UST/MPD/MIDI来源交叉编辑、HJPX字段完整往返、渲染后端反复切换不丢数据、登记/渲染零HJM写入、库修改与局部覆盖隔离、OTO cutoff正负号/编码/别名往返、已有Melodyne原生分段/连接/NSF合并组不退化。

## 5. 精确范围

- ex拼音转换：单字常用读音，小写无调，ü→v；不含多音字上下文，不自动把一个多字歌词拆成多个音符。
- ex单音符OTO编辑：单选打开，需要能解析已有音源与条目；新原生设计应允许先保留覆盖，再提示补全素材绑定。
- exOTO试听：整份录音从头播放；区域/循环试听属于素材管理器增强，不能误报为现成功能。
- ex与当前均不能仅凭有 `PitchAlgorithm::mld5` 或VST3实例创建函数就宣称真实Celemony渲染完成。
- 标准OTO不能完整表达所有原生分段、连接、曲线；格式导出应明示损失，原生HJPX仍完整保留。
