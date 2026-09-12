#include "I18n.h"
#include <array>
#include <unordered_map>

namespace hachi
{
namespace
{
using Row = std::array<const char*, 5>;
const std::unordered_map<std::string, Row> strings {
    { "app.title",       { "HachiShifter Next", "HachiShifter Next", "HachiShifter Next", "HachiShifter Next", "HachiShifter Next" } },
    { "file.open",       { "打开工程", "開啟工程", "プロジェクトを開く", "프로젝트 열기", "Open Project" } },
    { "file.save",       { "保存工程", "儲存工程", "プロジェクトを保存", "프로젝트 저장", "Save Project" } },
    { "file.saveAs",     { "工程另存为…", "工程另存新檔…", "プロジェクトを別名で保存…", "프로젝트 다른 이름으로 저장…", "Save Project As…" } },
    { "file.recent",     { "最近工程", "最近工程", "最近使ったプロジェクト", "최근 프로젝트", "Recent Projects" } },
    { "file.recentEmpty", { "没有最近工程", "沒有最近工程", "最近のプロジェクトはありません", "최근 프로젝트 없음", "No Recent Projects" } },
    { "file.export",     { "导出 WAV", "匯出 WAV", "WAVを書き出す", "WAV 내보내기", "Export WAV" } },
    { "file.exportLastRender", { "导出上次渲染音频", "匯出上次算繪音訊", "前回レンダリングした音声を書き出す", "마지막 렌더링 오디오 내보내기", "Export Last Render" } },
    { "export.lastRenderSuffix", { "上次渲染", "上次算繪", "前回のレンダリング", "마지막 렌더링", "last render" } },
    { "error.exportNoRender", { "还没有播放过框选的音符，没有可导出的渲染", "尚未播放過框選的音符，沒有可匯出的算繪", "選択したノートをまだ再生していないため、書き出せるレンダリングがありません", "선택한 음표를 재생한 적이 없어 내보낼 렌더링이 없습니다", "No marquee has been played yet, so there is no render to export" } },
    { "export.allTracks", { "全部轨道（每轨一个文件）", "全部音軌（每軌一個檔案）", "全トラック（トラックごとに1ファイル）", "모든 트랙 (트랙당 파일 1개)", "All tracks (one file each)" } },
    { "export.oneTrack", { "单个轨道", "單一音軌", "単一トラック", "단일 트랙", "A single track" } },
    { "export.untitledTrack", { "未命名轨道", "未命名音軌", "名称未設定トラック", "이름 없는 트랙", "Untitled track" } },
    { "status.exportRendering", { "正在渲染全曲音符…", "正在算繪全曲音符…", "曲全体のノートをレンダリング中…", "곡 전체 노트 렌더링 중…", "Rendering every note…" } },
    { "status.exportDone", { "已导出", "已匯出", "書き出しました", "내보냈습니다", "Exported" } },
    { "status.exportFiles", { "个文件", "個檔案", "ファイル", "개 파일", "file(s)" } },
    { "error.exportSilent", { "该轨道被静音或未独奏，导出会是静音", "該音軌被靜音或未獨奏，匯出會是靜音", "このトラックはミュート/非ソロのため無音になります", "이 트랙은 음소거 상태여서 무음으로 내보내집니다", "That track is muted or not soloed, so it would export silence" } },
    { "error.exportEmpty", { "没有可导出的轨道", "沒有可匯出的音軌", "書き出せるトラックがありません", "내보낼 트랙이 없습니다", "There is no track to export" } },
    { "file.audio",      { "导入音频", "匯入音訊", "オーディオを読み込む", "오디오 가져오기", "Import Audio" } },
    { "file.melodyne",   { "导入 Melodyne", "匯入 Melodyne", "Melodyneを読み込む", "Melodyne 가져오기", "Import Melodyne" } },
    { "file.new",        { "新建工程", "新增工程", "新規プロジェクト", "새 프로젝트", "New Project" } },
    { "file.midi",       { "导入 MIDI", "匯入 MIDI", "MIDIを読み込む", "MIDI 가져오기", "Import MIDI" } },
    { "file.ust",        { "导入 UST", "匯入 UST", "USTを読み込む", "UST 가져오기", "Import UST" } },
    { "error.ust",       { "无法读取 UST 工程", "無法讀取 UST 專案", "USTを読み込めません", "UST를 읽을 수 없습니다", "Could not read the UST" } },
    { "status.ustLoaded", { "UST 已导入为 UTAU 轨道", "UST 已匯入為 UTAU 軌道", "USTをUTAUトラックとして読み込みました", "UST를 UTAU 트랙으로 가져왔습니다", "UST imported as a UTAU track" } },
    { "file.exit",       { "退出", "結束", "終了", "종료", "Exit" } },
    { "file.settings",   { "设置…", "設定…", "設定…", "설정…", "Settings…" } },
    { "file.assets",     { "素材管理器…", "素材管理器…", "素材マネージャー…", "소재 관리자…", "Asset Manager…" } },
    { "menu.file",       { "文件", "檔案", "ファイル", "파일", "File" } },
    { "menu.edit",       { "编辑", "編輯", "編集", "편집", "Edit" } },
    { "menu.track",      { "轨道", "軌道", "トラック", "트랙", "Track" } },
    { "menu.view",       { "视图", "檢視", "表示", "보기", "View" } },
    { "menu.help",       { "帮助", "說明", "ヘルプ", "도움말", "Help" } },
    { "edit.undo",       { "撤销", "復原", "元に戻す", "실행 취소", "Undo" } },
    { "edit.redo",       { "重做", "重做", "やり直す", "다시 실행", "Redo" } },
    { "edit.selectAll",  { "全选音符", "全選音符", "全ノートを選択", "모든 노트 선택", "Select All Notes" } },
    { "edit.deselect", { "取消选择", "取消選取", "選択解除", "선택 해제", "Deselect" } },
    { "edit.copyNotes", { "复制所选音符", "複製所選音符", "選択ノートをコピー", "선택 음표 복사", "Copy Selected Notes" } },
    { "edit.cutNotes", { "剪切所选音符", "剪下所選音符", "選択ノートを切り取り", "선택 음표 잘라내기", "Cut Selected Notes" } },
    { "edit.pasteNotes", { "在播放位置粘贴音符", "在播放位置貼上音符", "再生位置にノートを貼り付け", "재생 위치에 음표 붙여넣기", "Paste Notes at Playhead" } },
    { "edit.pasteNotesAtOrigin", { "粘贴到原时间位置", "貼上到原時間位置", "元の時間位置に貼り付け", "원래 시간 위치에 붙여넣기", "Paste Notes at Original Time" } },
    { "edit.transposeCents", { "按音分移调…", "按音分移調…", "セントで移調…", "센트 단위 조옮김…", "Transpose by Cents…" } },
    { "edit.setPitch", { "设置音高…", "設定音高…", "ピッチを設定…", "피치 설정…", "Set Pitch…" } },
    { "edit.averagePitch", { "平均所选音高", "平均所選音高", "選択ピッチを平均化", "선택 피치 평균", "Average Selected Pitch" } },
    { "edit.quantizePitch", { "量化到半音", "量化到半音", "半音にクオンタイズ", "반음으로 퀀타이즈", "Quantize to Semitone" } },
    { "edit.cents", { "音分", "音分", "セント", "센트", "Cents" } },
    { "edit.midiNote", { "MIDI 音高", "MIDI 音高", "MIDI ノート", "MIDI 음높이", "MIDI Note" } },
    { "edit.copyClip",   { "复制所选采样", "複製所選取樣", "選択クリップをコピー", "선택 클립 복사", "Copy Selected Clip" } },
    { "edit.pasteClip",  { "在播放位置粘贴采样", "在播放位置貼上取樣", "再生位置にクリップを貼り付け", "재생 위치에 클립 붙여넣기", "Paste Clip at Playhead" } },
    { "edit.duplicateClip", { "紧接复制所选采样", "緊接複製所選取樣", "選択クリップを直後に複製", "선택 클립 바로 뒤에 복제", "Duplicate Selected Clip" } },
    { "view.zoomIn",     { "放大", "放大", "拡大", "확대", "Zoom In" } },
    { "view.zoomOut",    { "缩小", "縮小", "縮小", "축소", "Zoom Out" } },
    { "view.zoomFit",    { "适合工程", "符合工程", "プロジェクト全体", "프로젝트 맞춤", "Fit Project" } },
    { "view.showWaveforms", { "显示波形", "顯示波形", "波形を表示", "파형 표시", "Show Waveforms" } },
    { "view.vZoomIn", { "纵向放大", "縱向放大", "縦方向に拡大", "세로 확대", "Zoom In Vertically" } },
    { "view.vZoomOut", { "纵向缩小", "縱向縮小", "縦方向に縮小", "세로 축소", "Zoom Out Vertically" } },
    { "help.about",      { "关于 HachiShifter", "關於 HachiShifter", "HachiShifterについて", "HachiShifter 정보", "About HachiShifter" } },
    { "help.aboutText",  { "HachiShifter Next · JUCE/C++ 原生重构版", "HachiShifter Next · JUCE/C++ 原生重構版", "HachiShifter Next · JUCE/C++ ネイティブ版", "HachiShifter Next · JUCE/C++ 네이티브 버전", "HachiShifter Next · Native JUCE/C++ edition" } },
    { "transport.play",  { "播放", "播放", "再生", "재생", "Play" } },
    { "transport.pause", { "暂停", "暫停", "一時停止", "일시 정지", "Pause" } },
    { "transport.stop",  { "停止", "停止", "停止", "정지", "Stop" } },
    { "tool.main",       { "音符编辑", "音符編輯", "ノート編集", "노트 편집", "Note Edit" } },
    { "tool.wrench",     { "采样精修", "取樣精修", "サンプル編集", "샘플 정밀 편집", "Sample Edit" } },
    { "tool.draw",       { "自由绘制", "自由繪製", "フリーハンド", "자유 그리기", "Free Draw" } },
    { "tool.line",       { "直线工具", "直線工具", "直線ツール", "직선 도구", "Line Tool" } },
    { "tool.points",     { "标点音高工具", "標點音高工具", "ピッチポイント", "피치 포인트", "Pitch Points" } },
    { "stretch.unit", { "最小拉伸", "最小拉伸", "最小ストレッチ", "최소 늘이기", "Min Stretch" } },
    { "stretch.unitHelp", { "拉伸和移动音符时的最小单位，为一拍的 1/N。默认 1/64。", "拉伸和移動音符時的最小單位，為一拍的 1/N。預設 1/64。", "ノートの伸縮と移動の最小単位（1拍の 1/N）。既定は 1/64。", "노트 늘이기와 이동의 최소 단위(한 박의 1/N). 기본값 1/64.", "Smallest unit for stretching and moving notes, as 1/N of a beat. Default 1/64." } },
    { "pitchCurve.incomingSegment", { "上一点 → 当前点", "上一點 → 目前點", "前の点 → 現在の点", "이전 점 → 현재 점", "Previous Point → This Point" } },
    { "pitchCurve.noPrevious", { "音头点没有上一段", "音頭點沒有上一段", "先頭点には前の区間がありません", "첫 점에는 이전 구간이 없습니다", "The first point has no incoming segment" } },
    { "pitchCurve.natural", { "自然连续（默认）", "自然連續（預設）", "自然連続（既定）", "자연스러운 연속 (기본값)", "Natural Continuous (Default)" } },
    { "pitchCurve.linear", { "直线", "直線", "直線", "직선", "Linear" } },
    { "pitchCurve.smooth", { "S 型平滑", "S 型平滑", "S字スムーズ", "S자 부드럽게", "Smooth S" } },
    { "pitchCurve.easeIn", { "J形曲线（前慢后快）", "J形曲線（前慢後快）", "J字曲線（前半ゆっくり・後半速く）", "J자 곡선 (앞은 느리게, 뒤는 빠르게)", "J Curve (Slow Then Fast)" } },
    { "pitchCurve.easeOut", { "R形曲线（前快后慢）", "R形曲線（前快後慢）", "R字曲線（前半速く・後半ゆっくり）", "R자 곡선 (앞은 빠르게, 뒤는 느리게)", "R Curve (Fast Then Slow)" } },
    { "pitchCurve.customBezier", { "自定义贝塞尔…", "自訂貝茲曲線…", "カスタムベジェ…", "사용자 베지어…", "Custom Bezier…" } },
    { "pitchCurve.snapToNote", { "调整到标准音高", "調整到標準音高", "標準ピッチに合わせる", "표준 음높이로 맞추기", "Snap to Note Pitch" } },
    { "pitchCurve.setFrequency", { "输入音高…", "輸入音高…", "ピッチを入力…",
                                   "음높이 입력…", "Set Frequency…" } },
    { "pitchCurve.frequencyTitle", { "输入音高", "輸入音高", "ピッチを入力",
                                     "음높이 입력", "Set Point Frequency" } },
    { "pitchCurve.frequencyHelp", {
        "直接输入这个标点的基频 F0，单位赫兹（Hz）。440 即标准音 A4。\n只改这一个标点，相邻标点和音符本身都不动。",
        "直接輸入這個標點的基頻 F0，單位赫茲（Hz）。440 即標準音 A4。\n只改這一個標點，相鄰標點和音符本身都不動。",
        "このポイントの基本周波数 F0 をヘルツ（Hz）で直接入力します。440 が基準音 A4 です。\nこのポイントだけが動き、隣のポイントもノート自体も変わりません。",
        "이 점의 기본 주파수 F0을 헤르츠(Hz)로 직접 입력합니다. 440이 표준음 A4입니다.\n이 점만 바뀌고 이웃한 점과 노트 자체는 그대로입니다.",
        "Type this point's fundamental frequency in hertz; 440 is concert A4.\nOnly this point moves -- its neighbours and the note itself stay put." } },
    { "pitchCurve.frequencyField", { "F0（Hz）", "F0（Hz）", "F0（Hz）",
                                     "F0 (Hz)", "F0 (Hz)" } },
    { "pitchCurve.deletePoint", { "删除此标点", "刪除此標點", "このポイントを削除", "이 점 삭제", "Delete This Point" } },
    { "pitchCurve.bezierTitle", { "自定义贝塞尔曲线", "自訂貝茲曲線", "カスタムベジェ曲線", "사용자 베지어 곡선", "Custom Bezier Curve" } },
    { "pitchCurve.bezierHelp", { "设置归一化控制点。X 控制变化发生的时间，Y 控制音高推进量；Y 超出 0–1 可产生转音过冲。", "設定正規化控制點。X 控制變化時間，Y 控制音高推進量；Y 超出 0–1 可產生轉音過衝。", "正規化した制御点を設定します。X は変化のタイミング、Y はピッチの進行量です。0～1 外の Y でオーバーシュートを作れます。", "정규화된 제어점을 설정합니다. X는 변화 시점, Y는 피치 진행량이며 0–1 밖의 Y로 오버슈트를 만들 수 있습니다.", "Set normalised control points. X controls timing and Y controls pitch progress; Y outside 0–1 creates overshoot." } },
    { "pitchCurve.bezierX1", { "控制点 1 X", "控制點 1 X", "制御点 1 X", "제어점 1 X", "Control 1 X" } },
    { "pitchCurve.bezierY1", { "控制点 1 Y", "控制點 1 Y", "制御点 1 Y", "제어점 1 Y", "Control 1 Y" } },
    { "pitchCurve.bezierX2", { "控制点 2 X", "控制點 2 X", "制御点 2 X", "제어점 2 X", "Control 2 X" } },
    { "pitchCurve.bezierY2", { "控制点 2 Y", "控制點 2 Y", "制御点 2 Y", "제어점 2 Y", "Control 2 Y" } },
    { "tool.connect",    { "连接/分离音符", "連接/分離音符", "ノート接続/分離", "노트 연결/분리", "Connect/Separate Notes" } },
    { "editor.parameters", { "参数编辑器", "參數編輯器", "パラメータ", "매개변수 편집기", "Parameters" } },
    { "editor.smooth",   { "平滑", "平滑", "平滑化", "평활", "Smooth" } },
    { "editor.drift",    { "漂移修正", "漂移修正", "ドリフト補正", "드리프트 보정", "Drift Correction" } },
    { "editor.attackSpeed", { "辅音速度", "子音速度", "アタックスピード", "어택 속도", "Attack Speed" } },
    { "param.pitch",     { "音高", "音高", "ピッチ", "피치", "Pitch" } },
    { "param.drift",     { "漂移", "漂移", "ドリフト", "드리프트", "Drift" } },
    { "param.attack",    { "起音", "起音", "アタック", "어택", "Attack" } },
    { "param.breath",    { "呼吸", "呼吸", "ブレス", "브레스", "Breath" } },
    { "param.tension",   { "张力", "張力", "テンション", "텐션", "Tension" } },
    { "param.formant",   { "共振峰", "共振峰", "フォルマント", "포먼트", "Formant" } },
    { "param.volume",    { "音量", "音量", "音量", "음량", "Volume" } },
    { "param.robustPitchCurveShort", { "稳健线", "穩健線", "ロバスト", "강건선", "Robust" } },
    { "param.robustPitchCurve", { "稳健音高线（仅当前音符）", "穩健音高線（僅目前音符）", "ロバストピッチカーブ（現在のノートのみ）", "강건한 피치 곡선 (현재 음표만)", "Robust Pitch Curve (current note only)" } },
    { "beats.bar",       { "每小节", "每小節", "拍子", "마디 박자", "Beats" } },
    { "grid",            { "网格", "網格", "グリッド", "그리드", "Grid" } },
    { "base.scale",      { "基准调", "基準調", "基準キー", "기준 키", "Key" } },
    { "algo.pitch",      { "变调算法", "變調演算法", "ピッチアルゴリズム", "피치 알고리즘", "Pitch Algorithm" } },
    { "algo.stretch",    { "拉伸算法", "拉伸演算法", "タイムアルゴリズム", "타임 알고리즘", "Stretch Algorithm" } },
    { "algo.stretch.melodyneHybrid", { "Melodyne 混合拉伸", "Melodyne 混合拉伸", "Melodyne ハイブリッド", "Melodyne 하이브리드", "Melodyne Hybrid" } },
    { "algo.stretch.nsfVariableMel", { "NSF 可变 Hop Mel 先拼接后合成", "NSF 可變 Hop Mel 先拼接後合成", "NSF 可変 Hop Mel 結合後合成", "NSF 가변 Hop Mel 연결 후 합성", "NSF Variable-Hop Mel: Splice then Synthesize" } },
    { "algo.stretch.nsfShiftThenSplice", { "NSF 先变调后拼接", "NSF 先變調後拼接", "NSF ピッチ後結合", "NSF 피치 먼저 연결", "NSF Shift then Splice" } },
    { "algo.stretch.loop", { "循环拉伸", "循環拉伸", "ループストレッチ", "루프 스트레치", "Loop Stretch" } },
    { "algo.stretch.soundTouch", { "SoundTouch 拉伸", "SoundTouch 拉伸", "SoundTouch ストレッチ", "SoundTouch 스트레치", "SoundTouch Stretch" } },
    { "algo.order",      { "处理顺序", "處理順序", "処理順", "처리 순서", "Render Order" } },
    { "algo.order.processThenSplice", { "先合成后拼接", "先合成後拼接", "合成後結合", "합성 후 연결", "Process then Splice" } },
    { "algo.order.stretchSpliceThenPitch", { "先拼接后合成", "先拼接後合成", "結合後合成", "연결 후 합성", "Splice then Pitch" } },
    { "track.compose",   { "旋律", "旋律", "メロディック", "멜로디", "Compose" } },
    { "track.toggleCompose", { "切换旋律/普通音轨", "切換旋律/一般音軌", "メロディック/通常を切替", "멜로디/일반 전환", "Toggle Compose/Audio" } },
    { "track.addCompose", { "添加旋律轨道", "新增旋律軌道", "メロディックトラックを追加", "멜로디 트랙 추가", "Add Melodic Track" } },
    { "track.addAudio", { "添加普通音轨", "新增一般音軌", "通常トラックを追加", "일반 오디오 트랙 추가", "Add Audio Track" } },
    { "track.newHere", { "新建轨道", "新建軌道", "トラックを新規作成", "새 트랙", "New Track" } },
    { "track.newReference", { "新建素材轨道", "新建素材軌道", "素材トラックを新規作成",
                              "소재 트랙 새로 만들기", "New Reference Track" } },
    { "track.rename", { "重命名所选轨道…", "重新命名所選軌道…", "選択トラック名を変更…", "선택 트랙 이름 바꾸기…", "Rename Selected Track…" } },
    { "track.name", { "轨道名称", "軌道名稱", "トラック名", "트랙 이름", "Track Name" } },
    { "track.delete",    { "删除所选轨道", "刪除所選軌道", "選択トラックを削除", "선택 트랙 삭제", "Delete Selected Track" } },
    { "clip.delete",     { "删除所选采样", "刪除所選取樣", "選択クリップを削除", "선택 클립 삭제", "Delete Selected Clip" } },
    { "clip.mute",       { "静音所选采样", "靜音所選取樣", "選択クリップをミュート", "선택 클립 음소거", "Mute Selected Clip" } },
    { "clip.unmute",     { "取消采样静音", "取消取樣靜音", "クリップのミュート解除", "클립 음소거 해제", "Unmute Clip" } },
    { "clip.gain",       { "设置采样增益…", "設定取樣增益…", "クリップゲインを設定…", "클립 게인 설정…", "Set Clip Gain…" } },
    { "clip.gainDb",     { "增益（dB）", "增益（dB）", "ゲイン（dB）", "게인 (dB)", "Gain (dB)" } },
    { "track.audio",     { "普通音轨", "一般音軌", "通常トラック", "일반 트랙", "Audio Track" } },
    { "track.mute",      { "静音", "靜音", "ミュート", "음소거", "Mute" } },
    { "track.tip.compose", { "旋律轨道：开启后音符才会按音高渲染，钢琴窗里也才看得到这条轨道；关闭则作为普通音轨，素材原样播放",
                             "旋律軌道：開啟後音符才會依音高算繪，鋼琴窗中也才看得到這條軌道；關閉則作為一般音軌，素材原樣播放",
                             "メロディックトラック：オンのときだけノートが音高どおりにレンダリングされ、ピアノロールにも表示されます。オフなら通常トラックとして素材をそのまま再生します",
                             "멜로디 트랙: 켜면 노트가 음높이대로 렌더링되고 피아노 롤에도 표시됩니다. 끄면 일반 트랙으로 소재를 그대로 재생합니다",
                             "Compose track: only then are its notes rendered at their pitch and shown in the piano roll. Off, it is an audio track and the material plays as it is" } },
    { "track.tip.mute",  { "静音：这条轨道不发声，其余轨道照常",
                           "靜音：這條軌道不發聲，其餘軌道照常",
                           "ミュート：このトラックだけ音を出しません",
                           "음소거: 이 트랙만 소리가 나지 않습니다",
                           "Mute: this track is silent, the others play as usual" } },
    { "track.tip.solo",  { "独奏：只播放带独奏标记的轨道，其余全部静音",
                           "獨奏：只播放帶獨奏標記的軌道，其餘全部靜音",
                           "ソロ：ソロが付いたトラックだけを再生し、ほかはすべて無音になります",
                           "솔로: 솔로가 켜진 트랙만 재생하고 나머지는 모두 음소거됩니다",
                           "Solo: only soloed tracks play, everything else is silenced" } },
    { "track.tip.smooth", { "重叠淡化：相邻素材重叠处自动交叉淡化并把叠加音量归一，紧挨的接缝再补一段极短淡入，避免爆音",
                            "重疊淡化：相鄰素材重疊處自動交叉淡化並把疊加音量歸一，緊鄰的接縫再補一段極短淡入，避免爆音",
                            "重なりのフェード：素材が重なる部分を自動でクロスフェードし、合計音量をそろえます。隙間なく続く継ぎ目にはごく短いフェードを足してノイズを防ぎます",
                            "겹침 페이드: 소재가 겹치는 구간을 자동으로 크로스페이드하고 합쳐진 음량을 고르게 맞춥니다. 딱 붙은 이음매에는 아주 짧은 페이드를 넣어 잡음을 막습니다",
                            "Fade overlaps: overlapping material is crossfaded and its summed level evened out, and a very short fade is added at butt joins so they do not click" } },
    { "track.tip.normalize", { "音量归一：渲染后把音量匹配回原素材（NSF-HiFiGAN 按参考 Mel，其余后端按有声段 RMS）",
                               "音量歸一：算繪後把音量匹配回原素材（NSF-HiFiGAN 依參考 Mel，其餘後端依有聲段 RMS）",
                               "音量をそろえる：レンダリング後の音量を元の素材に合わせます（NSF-HiFiGANは参照メル、ほかのバックエンドは有声区間のRMS）",
                               "음량 정규화: 렌더링 후 음량을 원본 소재에 맞춥니다 (NSF-HiFiGAN은 참조 멜, 나머지 백엔드는 유성 구간 RMS)",
                               "Match level: after rendering, the level is matched back to the source (NSF-HiFiGAN against a reference mel, other backends by voiced RMS)" } },
    { "track.volume",    { "音量", "音量", "音量", "음량", "Vol" } },
    { "status.ready",    { "就绪", "就緒", "準備完了", "준비됨", "Ready" } },
    { "status.loading",  { "正在加载…", "正在載入…", "読み込み中…", "불러오는 중…", "Loading…" } },
    { "status.rendering", { "正在预渲染…", "正在預先算繪…", "プリレンダリング中…", "사전 렌더링 중…", "Pre-rendering…" } },
    { "status.analyzing", { "正在分析原始音高…", "正在分析原始音高…", "元ピッチを解析中…", "원본 피치 분석 중…", "Analysing source pitch…" } },
    { "status.analysisComplete", { "音高与音符分析完成", "音高與音符分析完成", "ピッチとノートの解析が完了しました", "피치 및 노트 분석 완료", "Pitch and note analysis complete" } },
    { "status.analysisSkipped", { "已保留现有音符数据", "已保留現有音符資料", "既存のノートデータを保持しました", "기존 노트 데이터를 유지했습니다", "Existing note data preserved" } },
    { "status.exporting", { "正在导出 WAV…", "正在匯出 WAV…", "WAVを書き出しています…", "WAV 내보내는 중…", "Exporting WAV…" } },
    { "status.clipCopied", { "已复制采样", "已複製取樣", "クリップをコピーしました", "클립을 복사했습니다", "Clip copied" } },
    { "status.clipPasted", { "已粘贴采样", "已貼上取樣", "クリップを貼り付けました", "클립을 붙여넣었습니다", "Clip pasted" } },
    { "status.notesCopied", { "已复制音符", "已複製音符", "ノートをコピーしました", "음표를 복사했습니다", "Notes copied" } },
    { "status.notesCut", { "已剪切音符", "已剪下音符", "ノートを切り取りました", "음표를 잘라냈습니다", "Notes cut" } },
    { "status.notesPasted", { "已粘贴音符", "已貼上音符", "ノートを貼り付けました", "음표를 붙여넣었습니다", "Notes pasted" } },
    { "status.projectOpened", { "已打开工程", "已開啟工程", "プロジェクトを開きました", "프로젝트를 열었습니다", "Project opened" } },
    { "status.projectSaved", { "已保存工程", "已儲存工程", "プロジェクトを保存しました", "프로젝트를 저장했습니다", "Project saved" } },
    { "status.midiPending", { "MIDI 导入器将在下一阶段接入", "MIDI 匯入器將於下一階段接入", "MIDIインポーターは次段階で接続します", "MIDI 가져오기는 다음 단계에서 연결됩니다", "MIDI importer will be connected in the next stage" } },
    { "status.noTracks", { "导入音频或工程以开始", "匯入音訊或工程以開始", "音声またはプロジェクトを読み込んでください", "오디오 또는 프로젝트를 가져오세요", "Import audio or a project to begin" } },
    { "edit.source",     { "原始采样编辑：此模式不允许拉伸", "原始取樣編輯：此模式不允許拉伸", "元サンプル編集：このモードではストレッチできません", "원본 샘플 편집: 이 모드에서는 늘이기를 사용할 수 없습니다", "Original sample edit: stretching is disabled" } },
    { "error.audio",     { "音频文件读取失败", "音訊檔案讀取失敗", "オーディオを読み込めません", "오디오 파일을 읽지 못했습니다", "Could not read audio file" } },
    { "error.midi",      { "MIDI 导入失败", "MIDI 匯入失敗", "MIDIの読み込みに失敗しました", "MIDI 가져오기에 실패했습니다", "MIDI import failed" } },
    { "error.mpd",       { "Melodyne 工程读取失败", "Melodyne 工程讀取失敗", "Melodyneプロジェクトを読み込めません", "Melodyne 프로젝트를 읽지 못했습니다", "Could not read Melodyne project" } },
    { "error.export",    { "音频导出失败", "音訊匯出失敗", "オーディオの書き出しに失敗しました", "오디오 내보내기 실패", "Audio export failed" } },
    { "warning.missingMedia", { "以下素材未找到", "找不到以下素材", "次の素材が見つかりません", "다음 미디어를 찾지 못했습니다", "The following media files were not found" } },
    { "mpd.stage.open", { "打开工程", "開啟工程", "プロジェクトを開く", "프로젝트 열기", "Opening project" } },
    { "mpd.stage.scan_container", { "扫描工程容器", "掃描工程容器", "コンテナを走査", "프로젝트 컨테이너 검사", "Scanning container" } },
    { "mpd.stage.decompress_graph", { "解压工程数据", "解壓工程資料", "データを展開", "프로젝트 데이터 압축 해제", "Decompressing graph" } },
    { "mpd.stage.read_tracks", { "读取轨道和 BPM", "讀取軌道與 BPM", "トラックとBPMを読込", "트랙 및 BPM 읽기", "Reading tracks and BPM" } },
    { "mpd.stage.create_tracks", { "恢复音符和编辑", "還原音符與編輯", "ノートと編集を復元", "노트 및 편집 복원", "Restoring notes and edits" } },
    { "mpd.stage.reanalyse_pitch", { "重新分析原始 F0", "重新分析原始 F0", "元のF0を再解析", "원본 F0 재분석", "Reanalysing source F0" } },
    { "mpd.stage.complete", { "完成", "完成", "完了", "완료", "Complete" } },
    { "mpd.compose.title", { "选择 Compose 轨道", "選擇 Compose 軌道", "Composeトラックを選択", "Compose 트랙 선택", "Choose Compose Tracks" } },
    { "mpd.compose.description", { "勾选需要恢复 Melodyne 音符和修音的旋律轨道；其余轨道按普通音频播放。", "勾選需要還原 Melodyne 音符與修音的旋律軌道；其餘軌道作為一般音訊播放。", "Melodyneのノート編集を復元する旋律トラックを選択します。その他は通常の音声トラックとして扱います。", "Melodyne 노트 편집을 복원할 멜로디 트랙을 선택하세요. 나머지는 일반 오디오 트랙으로 처리됩니다.", "Select melodic tracks whose Melodyne note edits should be restored. Other tracks remain regular audio tracks." } },
    { "dialog.import", { "导入", "匯入", "読み込む", "가져오기", "Import" } },
    { "dialog.cancel", { "取消", "取消", "キャンセル", "취소", "Cancel" } }
    ,{ "dialog.delete", { "删除", "刪除", "削除", "삭제", "Delete" } }
    ,{ "dialog.destructiveMessage", { "此操作会删除所选内容，是否继续？", "此操作會刪除所選內容，是否繼續？", "選択した内容を削除します。続行しますか？", "선택한 내용을 삭제합니다. 계속할까요?", "The selected content will be deleted. Continue?" } }
    ,{ "dialog.apply", { "应用", "套用", "適用", "적용", "Apply" } }
    ,{ "dialog.save", { "保存", "儲存", "保存", "저장", "Save" } }
    ,{ "dialog.discard", { "放弃更改", "放棄變更", "変更を破棄", "변경 내용 버리기", "Discard Changes" } }
    ,{ "dialog.unsavedTitle", { "工程尚未保存", "工程尚未儲存", "プロジェクトは未保存です", "프로젝트가 저장되지 않음", "Unsaved Project" } }
    ,{ "dialog.unsavedMessage", { "是否先保存当前工程的更改？", "是否先儲存目前工程的變更？", "現在のプロジェクトの変更を保存しますか？", "현재 프로젝트 변경 내용을 저장할까요?", "Save changes to the current project first?" } }
    ,{ "settings.title", { "设置", "設定", "設定", "설정", "Settings" } }
    ,{ "settings.interface", { "界面", "介面", "インターフェース", "인터페이스", "Interface" } }
    ,{ "settings.audio", { "音频", "音訊", "オーディオ", "오디오", "Audio" } }
    ,{ "settings.audioDevice", { "当前音频设备", "目前音訊裝置", "現在のオーディオデバイス", "현재 오디오 장치", "Current Audio Device" } }
    ,{ "settings.sampleRate", { "采样率", "取樣率", "サンプルレート", "샘플 레이트", "Sample Rate" } }
    ,{ "settings.bufferSize", { "缓冲区大小", "緩衝區大小", "バッファサイズ", "버퍼 크기", "Buffer Size" } }
    ,{ "settings.advancedAudio", { "选择输入、输出和驱动…", "選擇輸入、輸出與驅動…", "入出力とドライバーを選択…", "입출력 및 드라이버 선택…", "Choose Inputs, Outputs and Driver…" } }
    ,{ "settings.noAudioDevice", { "未选择音频设备", "尚未選擇音訊裝置", "オーディオデバイス未選択", "오디오 장치가 선택되지 않음", "No audio device selected" } }
    ,{ "settings.algorithm", { "算法", "演算法", "アルゴリズム", "알고리즘", "Algorithms" } }
    ,{ "settings.operation", { "操作", "操作", "操作", "조작", "Operations" } }
    ,{ "settings.import", { "文件导入", "檔案匯入", "ファイル読込", "파일 가져오기", "File Import" } }
    ,{ "settings.language", { "语言", "語言", "言語", "언어", "Language" } }
    ,{ "settings.theme", { "颜色主题", "色彩主題", "カラーテーマ", "색상 테마", "Colour Theme" } }
    ,{ "settings.themeDark", { "深色", "深色", "ダーク", "다크", "Dark" } }
    ,{ "settings.themeLight", { "浅色", "淺色", "ライト", "라이트", "Light" } }
    ,{ "settings.accent", { "主色（Hex）", "主色（Hex）", "アクセント（Hex）", "강조색 (Hex)", "Accent (Hex)" } }
    ,{ "settings.accentLight", { "浅主色（Hex）", "淺主色（Hex）", "明るい主色（Hex）", "밝은 강조색 (Hex)", "Light Accent (Hex)" } }
    ,{ "settings.noteColour", { "音符色（Hex）", "音符色（Hex）", "ノート色（Hex）", "노트 색상 (Hex)", "Note Colour (Hex)" } }
    ,{ "settings.showNoteLabels", { "显示已标注的发音/别名", "顯示已標註的發音/別名", "注釈済みの発音・別名を表示", "표시된 발음/별칭 표시", "Show annotated pronunciation/alias" } }
    ,{ "settings.uiScale", { "界面缩放", "介面縮放", "UIスケール", "UI 배율", "UI Scale" } }
    ,{ "settings.gamePath", { "GAME 模型目录", "GAME 模型目錄", "GAMEモデルフォルダー", "GAME 모델 폴더", "GAME Model Directory" } }
    ,{ "settings.gameModel", { "GAME 默认模型", "GAME 預設模型", "GAME既定モデル", "GAME 기본 모델", "Default GAME Model" } }
    ,{ "settings.fcpePath", { "FCPE 模型或目录", "FCPE 模型或目錄", "FCPEモデルまたはフォルダー", "FCPE 모델 또는 폴더", "FCPE Model or Directory" } }
    ,{ "settings.hifiganPath", { "HiFi-GAN 模型目录", "HiFi-GAN 模型目錄", "HiFi-GANモデルフォルダー", "HiFi-GAN 모델 폴더", "HiFi-GAN Model Directory" } }
    ,{ "settings.inference", { "推理方式", "推理方式", "推論バックエンド", "추론 백엔드", "Inference Backend" } }
    ,{ "settings.device", { "推理设备", "推理裝置", "推論デバイス", "추론 장치", "Inference Device" } }
    ,{ "settings.auto", { "自动", "自動", "自動", "자동", "Auto" } }
    ,{ "settings.utauResampler", { "UTAU 重采样器（可选，留空则内置变调）", "UTAU 重取樣器（可選，留空則內建變調）", "UTAUリサンプラー（任意、空欄は内蔵処理）", "UTAU 리샘플러 (선택, 비우면 내장 처리)", "UTAU Resampler (optional; built-in fallback)" } }
    ,{ "settings.shortcuts", { "快捷键方案", "快速鍵配置", "ショートカット方式", "단축키 방식", "Shortcut Scheme" } }
    ,{ "settings.wheel", { "鼠标滚轮", "滑鼠滾輪", "マウスホイール", "마우스 휠", "Mouse Wheel" } }
    ,{ "settings.wheelZoom", { "缩放", "縮放", "ズーム", "확대/축소", "Zoom" } }
    ,{ "settings.wheelScroll", { "滚动", "捲動", "スクロール", "스크롤", "Scroll" } }
    ,{ "settings.spacePlayback", { "空格键播放/暂停", "空白鍵播放/暫停", "スペースで再生/一時停止", "스페이스바 재생/일시정지", "Space toggles playback" } }
    ,{ "settings.confirmDestructive", { "删除前确认", "刪除前確認", "削除前に確認", "삭제 전 확인", "Confirm before delete" } }
    ,{ "settings.melodyneCompose", { "Melodyne Compose 默认方式", "Melodyne Compose 預設方式", "Melodyne Composeの既定値", "Melodyne Compose 기본값", "Default Melodyne Compose" } }
    ,{ "settings.composeAsk", { "每次询问", "每次詢問", "毎回確認", "매번 확인", "Ask Every Time" } }
    ,{ "settings.composeMelodic", { "旋律轨道", "旋律軌道", "メロディックトラック", "멜로디 트랙", "Melodic Tracks" } }
    ,{ "settings.composeAll", { "全部轨道", "全部軌道", "すべてのトラック", "모든 트랙", "All Tracks" } }
    ,{ "settings.composeAudio", { "仅普通音轨", "僅一般音軌", "通常音声のみ", "일반 오디오만", "Audio Only" } }
    ,{ "settings.melodynePitch", { "Melodyne 原音高来源", "Melodyne 原音高來源", "Melodyne元ピッチの取得元", "Melodyne 원본 피치 소스", "Melodyne Source Pitch" } }
    ,{ "settings.pitchProject", { "工程记录", "工程記錄", "プロジェクトデータ", "프로젝트 데이터", "Project Data" } }
    ,{ "settings.pitchReanalyse", { "GAME + FCPE（无模型时使用原生分析）", "GAME + FCPE（無模型時使用原生分析）", "GAME + FCPE（モデルなしはネイティブ解析）", "GAME + FCPE (모델 미포함 시 네이티브 분석)", "GAME + FCPE (native fallback without models)" } }
    ,{ "settings.importAlgorithm", { "导入工程默认变调算法", "匯入工程預設變調演算法", "読込時の既定ピッチアルゴリズム", "가져오기 기본 피치 알고리즘", "Default Import Pitch Algorithm" } }
    ,{ "settings.importStretchAlgorithm", { "导入工程默认拉伸算法", "匯入工程預設拉伸演算法", "読込時の既定タイムストレッチ", "가져오기 기본 타임 스트레치", "Default Import Stretch Algorithm" } }
    ,{ "settings.preserveEdits", { "保留工程中的修音、Attack、音量和音色编辑", "保留工程中的修音、Attack、音量與音色編輯", "ピッチ補正・Attack・音量・音色の編集を保持", "피치 보정·Attack·음량·음색 편집 유지", "Preserve tuning, Attack, level and timbre edits" } }
    ,{ "settings.recursiveMedia", { "递归查找缺失素材", "遞迴尋找遺失素材", "不足素材を再帰検索", "누락 미디어 재귀 검색", "Search recursively for missing media" } }
    ,{ "sample.alias", { "别名", "別名", "エイリアス", "별칭", "Alias" } }
    ,{ "sample.start", { "起点", "起點", "開始", "시작", "Start" } }
    ,{ "sample.end", { "终点", "終點", "終了", "끝", "End" } }
    ,{ "sample.alignment", { "对齐", "對齊", "整列", "정렬", "Align" } }
    ,{ "sample.fixed", { "辅音", "子音", "子音", "자음", "Fixed" } }
    ,{ "sample.save", { "保存设定", "儲存設定", "設定を保存", "설정 저장", "Save Settings" } }
    ,{ "sample.saved", { "音频设定已保存", "音訊設定已儲存", "音声設定を保存しました", "오디오 설정 저장됨", "Audio settings saved" } }
    ,{ "sample.importOto", { "读取 oto", "讀取 oto", "oto読込", "oto 읽기", "Import oto" } }
    ,{ "sample.exportOto", { "导出 oto", "匯出 oto", "oto書出", "oto 내보내기", "Export oto" } }
    ,{ "asset.title", { "素材管理器", "素材管理器", "素材マネージャー", "소재 관리자", "Asset Manager" } }
    ,{ "asset.register", { "注册音频素材", "註冊音訊素材", "音声素材を登録", "오디오 소재 등록", "Register Audio Assets" } }
    ,{ "asset.utau", { "导入 UTAU 音源库", "匯入 UTAU 音源庫", "UTAU 音源を読み込む", "UTAU 음원 가져오기", "Import UTAU Voicebank" } }
    ,{ "asset.utauDone", { "已注册 {files} 个音频，生成 {sidecars} 个 HJM 文件、{regions} 个分段。", "已註冊 {files} 個音訊，產生 {sidecars} 個 HJM 檔案、{regions} 個分段。", "{files} 件の音声を登録し、{sidecars} 件の HJM と {regions} 件の区間を作成しました。", "오디오 {files}개를 등록하고 HJM {sidecars}개와 구간 {regions}개를 생성했습니다.", "Registered {files} audio files and generated {sidecars} HJM files with {regions} regions." } }
    ,{ "asset.remove", { "移除", "移除", "削除", "제거", "Remove" } }
    ,{ "asset.empty", { "把音频拖入此处注册；注册后可拖到时间线或钢琴卷帘。", "將音訊拖入此處註冊；註冊後可拖到時間軸或鋼琴捲簾。", "音声をここへドロップして登録し、タイムラインまたはピアノロールへドラッグできます。", "오디오를 여기에 놓아 등록한 뒤 타임라인이나 피아노 롤로 끌 수 있습니다.", "Drop audio here to register it, then drag it to the timeline or piano roll." } }
    ,{ "settings.browse", { "浏览…", "瀏覽…", "参照…", "찾아보기…", "Browse…" } }
    ,{ "settings.utauVoicebank", { "UTAU 默认音源文件夹", "UTAU 預設音源資料夾", "UTAU 既定音源フォルダー", "UTAU 기본 음원 폴더", "Default UTAU Voicebank" } }
    ,{ "settings.utauWavtool", { "UTAU 合成器 / wavtool（可选）", "UTAU 合成器 / wavtool（可選）", "UTAU 合成ツール / wavtool（任意）", "UTAU 합성기 / wavtool (선택)", "UTAU Synthesis Tool / wavtool (optional)" } }
    ,{ "settings.chooseUtauVoicebank", { "选择 UTAU 音源文件夹", "選擇 UTAU 音源資料夾", "UTAU 音源フォルダーを選択", "UTAU 음원 폴더 선택", "Choose UTAU Voicebank Folder" } }
    ,{ "settings.chooseUtauWavtool", { "选择 UTAU 合成器 / wavtool", "選擇 UTAU 合成器 / wavtool", "UTAU 合成ツール / wavtool を選択", "UTAU 합성기 / wavtool 선택", "Choose UTAU Synthesis Tool / wavtool" } }
    ,{ "settings.chooseUtauResampler", { "选择 UTAU 重采样器", "選擇 UTAU 重取樣器", "UTAU リサンプラーを選択", "UTAU 리샘플러 선택", "Choose UTAU Resampler" } }
};
}

I18n::I18n()
{
    const auto locale = juce::SystemStats::getUserLanguage().toLowerCase();
    if (locale.startsWith("ja")) language = Language::jaJP;
    else if (locale.startsWith("ko")) language = Language::koKR;
    else if (locale.contains("tw") || locale.contains("hk") || locale.contains("hant")) language = Language::zhTW;
    else if (!locale.startsWith("zh")) language = Language::enUS;
}

juce::String I18n::text(const juce::String& key) const
{
    const auto found = strings.find(key.toStdString());
    if (found == strings.end()) return key;
    return juce::String::fromUTF8(found->second[static_cast<std::size_t>(language)]);
}
}
