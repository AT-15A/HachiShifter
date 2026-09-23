# HachiShifter 原生统一工作流 TODO

## 本轮已审核发布检查

- 用户已通过 UI 审核，并授权最新合并渲染验证通过后推送 `new`。
- 最新构建重新导入 teio，精确独奏 mm（81 片段），29 个同源连续组先拼接后送入真实 NSF ONNX CPU；没有 fallback。
- 输出 `teio-mm-nsf-merged-ui-approved.wav`：48000 Hz、2 声道、24-bit、481489 帧，10.031020833 秒。
- 与此前试听通过的 `teio-mm-nsf-stretch-splice-then-pitch.wav` 逐采样一致，最大差值 0；PCM SHA256 为 `e9b2f66897a58210b184ad1c3e27576f6f61675f4c959b3317c772cf3636bbfa`。
- 这是当前可用实现的审核结果，不代表整个原生重构计划全部完成；重心估算、异源整组渲染、历史翻译清理等待办仍保留。

## 内部分界与速度语义修正

- 音符内部导入分界对应 HJM alignment（先行结束位置），不对应相邻音符接缝，也不等同 fixed_duration。
- 无发音信息仍标 `-`；可根据内部边界生成辅音/元音类别，不再因 alias 未知把整段都置为 unknown。
- 当前固定段终点使用有声源时间位置的均值估算（无可用有声点时使用区间中点）；它是 HachiShifter 的粗略规划，不是已复现的 Melodyne 重心算法。分区来源标 estimated。
- 原生分区显示通过 sourceTimeMap 逆映射到工程坐标；录音 onset 已含在 clip 内，不能再向前扩展一份先行音频。
- 包络新默认开启，读取导入 fade/音量包络；用户可以关闭。
- 普通录音音符使用同一辅音速度数字控件；修改同步重映射时图、F0采样时刻、锚点和包络时间，源音高值、源选区和总时长保持不变，支持撤销。
- teio/mm 转换检查：81 音符、47 个内部边界、47 辅音段、162 元音子段，81 个未命名 alias。
- 拉伸选择器始终显示；WORLD/LLSM2 等目前只开放实际使用的算法原生时图拉伸，NSF 专属 Mel 项不出现在其他后端。额外通用拉伸算法仍需独立实现，不能只添加无效菜单。
- 已接入本轮显示菜单与辅音速度文案翻译；其他历史硬编码文案仍需后续清理。

## 当前后端修复与验收状态

- 用户确认 `mm` 新 NSF 输出的跑调已解决，剩余听感瑕疵待比较两种渲染顺序。
- 已解除 NSF 合并调度与 Melodyne Provider 实验禁用开关的错误关联；失败会结束全部待分发切片并传播错误。
- `mm` 两种 NSF 顺序已测试，固定使用相同模型和 variable-mel-hop：
  - `teio-mm-nsf-process-then-splice.wav`：先逐片段 HiFiGAN 后拼接，合并组 0。
  - `teio-mm-nsf-stretch-splice-then-pitch.wav`：同源连续连接组先拼接后 HiFiGAN，合并组 29。
- 两份 WAV 均为 48 kHz / stereo / 24-bit，10.031020833 秒；并非同一音频。日志 `teio-mm-render-orders.json` 与音频位于 sample1/r。
- `--smoke-render-order` 验证真实 UI 下拉框可见、两个选项写入与回读正确，合并/独立调度分别为 1/0。
- 当前合并范围是同源连续连接组；异源整轨一次推理尚未实现，不能宣称已支持。

- 轨道更正：必须使用 `teio.mpd` 中的 `mm`（81 个片段）。之前 `main_MixDown` 的对比不能作为验收结果。
- 已修复 WSL 导入 Windows 盘符路径：优先解析 `D:/...` 为 `/mnt/d/...`，再尝试同名素材搜索。此前缺少该转换导致 `mm` 被整轨丢弃。
- 重读后共有 4 条有素材轨道、255 个片段；验收导出只启用 `mm`，其余静音。
- `teio-mm-strict-nsf-hifigan.wav`：真实 ONNX CPU / variable-mel-hop；`teio-mm-strict-world.wav`：WORLD。LLSM2 仍报告片段失败，未输出替代算法音频。
- `teio-mm-pitch-comparison.json` 是与参考主唱的粗略相关法 F0 诊断，不代表跑调问题已解决；仍需人工试听。

- 新测试输入为 `D:\hjm\ms\teio.mpd`，参考主唱为 `D:\hjm\ms\p2_vocal_main.wav`。
- 默认优先 NSF-HiFiGAN，无模型时新建默认选择 LLSM2；显式选择不可用后端必须报错。
- MLD5/MLD3 禁用，vslib 原生库尚未接入，不再将 Signalsmith 输出标为 vslib。
- 真正的 NSF 模型位于 `C:\Users\funny\Desktop\HachiShifter\models\nsf_hifigan`。
- 已执行 teio 的 NSF ONNX CPU、WORLD 严格渲染；LLSM2 有片段拒绝渲染，应继续定位，不能回退冒充成功。
- 输出在 `C:\Users\funny\Downloads\sample1\r\teio-strict-*.wav`；单独主唱为 `teio-main-strict-nsf-hifigan.wav`。
- 音高对齐仍在调查，自动相关估计只作诊断，不能作为听感通过结论。
- 用户人工确认之前，不提交推送或触发 GitHub Actions。

## 总原则

- UTAU、UST、OTO、MIDI、Melodyne MPD 都是输入/交换格式。
- 导入完成后统一转换为 HachiShifter 原生工程和 HJM 素材标注。
- 原生工程不区分 UTAU 模式和 Melodyne 模式，界面、编辑器、缓存和渲染请求只有一套。
- 工程导入完成后必须能够直接播放；不能因为缺少外部格式上下文而产生静音或等待状态。
- 外部后端可以保留为渲染实现，但不能让外部工程格式成为内部数据模型。

## HJM 标注规范

- 使用项目既有的独立素材标注文件：`audio-file.hjm.csv`。
- 为 HJM 标注增加版本号和可扩展字段，目标为 HJM v2。
- HJM 区域至少保存：源区域起止、别名、音高中心、原始 F0、目标音高、辅元音边界、先行、overlap、音量包络和拉伸规划。
- HJM 支持无限数量的区域和区域内部 segment，不受 UTAU OTO 四段限制。
- Melodyne 没有发音标记：其导入得到的未知/未确认分段统一使用 `-` 表示。
- `-` 表示未知，不表示静音，也不应被自动当作可发音 alias；用户可以在标注工具中替换为实际发音。
- HJM 中保留 `source=melodyne`、`source=utau`、`source=estimated`、`source=user` 等来源信息；不存在的信息自动识别时标记为 estimated，无法可靠识别时使用保留字段而不是虚构结果。
- 自动推断结果必须可视化、可编辑、可恢复默认。

## 发音类型

统一发音类型至少包括：

- `-`：未知/未确认分段。
- 普通辅音。
- 元音。
- `transition`：衔接音，默认用下划线 `_` 表示。
- 静音、气声、噪声、结尾音和用户自定义类型。

### 衔接音识别

- 对 Melodyne 音符序列进行保守分析。
- 音频时间非常短、无 F0 的比例较大、且位于两个有声片段之间时，候选识别为衔接音。
- 衔接音的默认别名为 `_`，留给用户进一步完善。
- 不满足条件时保持 `-`，避免把真正的辅音或静音误归类。
- 用户确认后才能转换为具体 CV/VC/VV 发音；确认结果写入 HJM，不覆盖原始分析数据。

### CVVC 示例

连续音 `wa ta shi` 可转换为类似以下 HJM 区域序列：

```text
wa  a_t  ta  a_sh  shi
```

其中：

- `wa`、`ta`、`shi` 是普通 CV 区域。
- `a_t`、`a_sh` 是衔接音/VC 或 VV-辅音过渡区域。
- 如果来源只有 Melodyne 而无法确定具体发音，先保存为 `_`，不强行生成 `a_t` 或 `a_sh`。
- UTAU CVVC 标注导入时可直接使用已知 alias 和类型，并统一写入 HJM。

## 导入流程

### 直接导入音频

1. 建立素材对象并读取音频属性。
2. 自动生成粗略 HJM 区域和 F0/voiced 分析。
3. 无法确认的区域使用 `-`。
4. 短时、无 F0 比例大的中间片段生成 `_` 衔接音候选。
5. 立即写入 `.hjm.csv`。
6. 素材进入素材管理器，并可以马上播放和标注。

### Melodyne MPD 导入

1. 读取音频引用、源时间范围、音符、连续 F0、音量和连接信息。
2. 转换为 HJM 区域和原生音符。
3. Melodyne 没有发音信息的区域使用 `-`。
4. 根据时长、voiced 比例和相邻有声区域生成 `_` 衔接音候选。
5. 辅音/元音边界转换为 HJM 先行和 overlap。
6. Melodyne 音量变化转换为 HJM 音量包络。
7. 自动估算的拉伸区写入 HJM，但标记为 estimated。
8. 转换完成后立即建立原生播放请求，不依赖 Melodyne 进程。
9. 已存在 HJM 标注时按当前测试策略直接覆盖；正式流程再恢复冲突选择。

### UST / OTO 导入

1. UST 音符、歌词、tempo、Mode-2 曲线、envelope、flags 转换为原生音符。
2. OTO 的 offset、consonant、cutoff、preutterance、overlap 转换为 HJM 区域标注。
3. CVVC/多区域 alias 和类型直接写入 HJM segment。
4. OTO 不再作为原生工程运行时依赖。
5. 找不到音源区域时保留原生未绑定音符，并使用 `-` 或 `_` 标记待完善，而不是丢失音符。
6. 转换结束后原生 UTAU 渲染后端即可直接播放。

## 原生编辑流程

- 用户在统一钢琴窗中编辑音符、源素材区域、alias、音高、音高线、颤音和音量包络。
- 素材标注工具修改 HJM，并使所有引用该区域的音符缓存失效。
- 用户可从素材管理器把已标注区域拖入钢琴窗，生成新的原生音符实例。
- 原生音符保留素材来源，但不保留 UTAU/Melodyne 内部模式。
- 强制连接工具可以连接不同素材、不同文件和不同 HJM 区域。
- 连接只记录原生 `NativeConnection`，不伪造 OTO 或 Melodyne join。
- 每个音符保留原始 F0 与目标 F0，用户编辑只修改目标层。

## 原生渲染

- 所有后端接收统一的 NativeRenderRequest。
- 请求包含素材区域、源时间图、目标时长、目标 F0、音量包络、stretch segments 和连接上下文。
- 未完成的 Melodyne 自研导入/合并算法继续默认禁用。
- 原生 HachiShifter 播放优先使用已转换的本地 HJM/Native 数据。
- 不要求 Melodyne 本体常驻才能播放已经转换的工程。

## 实施顺序

1. [x] 扩展 HJM v2 的 segment/type/source/confidence 字段。
2. [x] 建立统一 NativeMaterial、NativeAnnotation、NativeRegion、NativeNote、NativeConnection。
3. [x] 将 OTO/UST 转换器输出改为 HJM + Native 数据。
4. [x] 将 Melodyne 导入器输出改为 HJM + Native 数据，未知使用 `-`，衔接音候选使用 `_`。
5. [x] 统一工程加载、保存、缓存和直接播放流程的基础字段。
6. 删除钢琴窗和主界面的 UTAU/Melodyne 两套布局分支。
7. 接入素材管理器拖放和标注即时保存。
8. 实现跨素材强制连接和统一音量/音高过渡。
9. 添加导入、转换、直接播放、标注修改、保存重载和回退测试。
10. 完成验证后再考虑启用 Melodyne Provider 或解除自研实验算法禁用。

当前验证：`--smoke-native-hjm`、`--smoke-note-menu-split`、
`--smoke-view-menu` 已通过；完整 Windows MPD 全算法导入/导出验收仍待
统一工程重构完成后执行。

## 验收条件

- Melodyne MPD 导入后无需 Melodyne 进程即可播放。
- UST/OTO 导入后无需 OTO 文件即可播放。
- 所有未知分段明确显示为 `-`，衔接音候选明确显示为 `_`。
- 用户可把 `-`/`_` 改成真实 alias 和发音类型并自动保存。
- HJM 可保存比 OTO 更细的无限元音分段。
- UTAU 和 Melodyne 输入最终生成同一种原生工程和同一种界面。
- 不同素材间的连接、滑音、音量过渡和源 F0 保持独立可调。
- 工程重载后无需重新导入 UST、OTO 或 MPD 即可恢复编辑和播放。

## 最终 MPD 全算法导入/导出验收

测试输入：

```text
/mnt/c/Users/funny/Downloads/sample1/ts.mpd
```

Windows 对应路径：

```text
C:\Users\funny\Downloads\sample1\ts.mpd
```

输出目录：

```text
/mnt/c/Users/funny/Downloads/sample1/r
```

在统一原生转换完成后执行：

1. 导入 `ts.mpd`。
2. 生成/覆盖对应 HJM v2 素材标注。
3. 保存并重新加载 HachiShifter 原生工程。
4. 分别导出每一种当前可用的非 Melodyne 音频修音算法。
5. 每种算法单独输出 WAV 和日志，不能覆盖其它算法的结果。
6. 检查输出文件可读、时长正确、采样率/声道正确、非静音，并且未知 `-` 与衔接 `_` 标注不会导致整段丢失。
7. 检查 HJM segment、先行、overlap、音量包络、源 F0、目标音高和 NativeConnection 在保存/重载后仍存在。

测试算法范围：

- MLD5。
- NSF-HiFiGAN。
- WORLD。
- VocalShifter/vslib。
- LLSM2。
- Windows 环境中可用的 UTAU resampler。

明确不测试 Melodyne 原生音频算法、尚未完成的 Melodyne Provider 和未验证的合并渲染实验路径。

建议输出文件名：

```text
ts-mld5.wav
ts-nsf-hifigan.wav
ts-world.wav
ts-vslib.wav
ts-llsm2.wav
ts-utau.wav
```
