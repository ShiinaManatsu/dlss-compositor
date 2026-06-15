import 'package:flutter/material.dart';

class AppStrings {
  AppStrings(this.locale);

  final Locale locale;

  bool get isZh => locale.languageCode.toLowerCase().startsWith('zh');

  static AppStrings of(BuildContext context) {
    return AppStrings(Localizations.localeOf(context));
  }

  String get appTitle => 'DLSS Compositor';
  String get language => isZh ? '语言' : 'Language';
  String get languageSystem => isZh ? '跟随系统' : 'System';
  String get languageEnglish => 'English';
  String get languageChinese => '简体中文';
  String get openDlssConfig => 'Open dlssconfig.json';
  String get openDlssConfigDialogTitle =>
      isZh ? '打开 dlssconfig.json' : 'Open dlssconfig.json';
  String get settings => isZh ? '设置' : 'Settings';
  String get valid => isZh ? '有效' : 'Valid';
  String get qualityFallback => isZh ? '未知' : 'N/A';
  String get hdr => 'HDR';
  String fps(int value) => '$value fps';
  String frames(int value) => isZh ? '$value 帧' : '$value frames';
  String inputResolution(Object? width, Object? height) => 'In: ${width ?? '?'}x${height ?? '?'}';
  String outputResolution(Object? width, Object? height) => 'Out: ${width ?? '?'}x${height ?? '?'}';
  String get unavailableOnThisDevice => isZh ? '当前设备不可用' : 'Unavailable on this device';
  String get startProcessing => isZh ? '开始处理' : 'START PROCESSING';
  String get cancel => isZh ? '取消' : 'CANCEL';
  String get cancellingUpper => isZh ? '正在取消...' : 'CANCELLING...';
  String get cancelling => isZh ? '正在取消...' : 'Cancelling...';
  String get ready => isZh ? '就绪' : 'Ready';
  String get noEntries => isZh ? '暂无条目' : 'No entries';
  String entries(int value) => isZh ? '$value 条' : '$value entries';
  String get noTerminalOutputYet => isZh ? '暂无终端输出' : 'No terminal output yet';
  String get livePreview => isZh ? '实时预览' : 'Live Preview';
  String get processing => isZh ? '处理中' : 'PROCESSING';
  String get completed => isZh ? '已完成' : 'COMPLETED';
  String get error => isZh ? '错误' : 'ERROR';
  String get cancellingTitle => isZh ? '正在取消' : 'CANCELLING';
  String eta(String value) => isZh ? '剩余 $value' : 'ETA $value';
  String get dlssPipeline => 'DLSS Pipeline';
  String get diskOutput => isZh ? '磁盘写出' : 'Disk Output';
  String get terminalLog => isZh ? '终端日志' : 'TERMINAL LOG';
  String get processCompleted => isZh ? '处理完成' : 'PROCESS COMPLETED';
  String renderedFramesInSeconds(int frames, int seconds) =>
      isZh ? '$frames 帧已渲染，用时 ${seconds}s' : 'Rendered $frames frames in ${seconds}s';
  String get openFolder => isZh ? '打开文件夹' : 'Open Folder';
  String get noValidConfigTitle => isZh ? '没有有效的 dlssconfig.json' : 'No valid dlssconfig.json';
  String get noValidConfigMessage =>
      isZh ? '开始前请先选择一个有效的 dlssconfig.json。' : 'Please select a valid dlssconfig.json before starting.';
  String get invalidFilenameTemplateTitle => isZh ? '文件名模板无效' : 'Invalid filename template';
  String get filenameTemplateRequired => isZh ? '文件名模板不能为空' : 'Filename template is required';
  String get filenameTemplateNoSeparators =>
      isZh ? '文件名模板不能包含路径分隔符' : 'Filename template must not contain path separators';
  String get outputDirectoryRequiredTitle => isZh ? '需要输出目录' : 'Output directory required';
  String get outputDirectoryRequiredMessage =>
      isZh ? '开始前请选择输出目录。' : 'Please select an output directory before starting.';
  String get backendRejectedTaskTitle => isZh ? '后端拒绝了任务' : 'Backend rejected task';
  String get backendRejectedTaskMessage => isZh ? '设置任务参数失败。' : 'Failed to set task parameters.';
  String get backendUnavailableTitle => isZh ? '处理后端不可用' : 'Processing backend unavailable';
  String get backendUnavailableMessage =>
      isZh ? '处理后端无法启动，请检查根目录日志。' : 'The processing backend could not be started. Check the root logs for details.';
  String get cancelFailedTitle => isZh ? '取消失败' : 'Cancel failed';
  String get cancelRejectedMessage =>
      isZh ? '处理后端没有接受取消请求。' : 'The processing backend did not accept the cancel request.';
  String get cancelFailedMessage => isZh ? '取消请求失败。' : 'The cancel request failed.';
  String get outputCollisionsTitle => isZh ? '检测到输出冲突' : 'Output collisions detected';
  String outputCollisionsMessage(int count, String mode) =>
      isZh ? '$count 个输出文件可能冲突。当前模式: $mode。是否继续？' : '$count output files may conflict. Mode: $mode. Continue?';
  String get continueLabel => isZh ? '继续' : 'Continue';
  String get outputFolderNotFoundTitle => isZh ? '找不到输出文件夹' : 'Output folder not found';
  String get outputFolderNotFoundMessage => isZh ? '输出文件夹还不存在。' : 'The output folder does not exist yet.';
  String get failedOpenOutputFolderTitle => isZh ? '打开输出文件夹失败' : 'Failed to open output folder';
  String get failedOpenOutputFolderMessage => isZh ? '无法启动系统文件管理器。' : 'Could not launch the system file manager.';
  String get backendErrorTitle => isZh ? '后端错误' : 'Backend error';
  String get backendErrorMessage => isZh ? '后端报告了运行时错误。' : 'The backend reported a runtime error.';
  String get backendStreamErrorTitle => isZh ? '后端流错误' : 'Backend stream error';
  String get backendStreamErrorMessage => isZh ? '后端事件流失败。' : 'The backend event stream failed.';
  String get invalidConfigTitle => isZh ? '无效的 dlssconfig.json' : 'Invalid dlssconfig.json';
  String get failedOpenConfigTitle => isZh ? '打开 dlssconfig.json 失败' : 'Failed to open dlssconfig.json';
  String get failedOpenConfigMessage => isZh ? '选中的文件无法解析。' : 'The selected file could not be parsed.';
  String invalidVersion(Object? got) =>
      isZh ? '版本无效（期望 1，实际 ${got ?? 'null'}）' : 'Invalid version (expected 1, got ${got ?? 'null'})';
  String missingRequiredField(String field) => isZh ? '缺少必填字段: $field' : 'Missing required field: $field';
  String get selectFolder => isZh ? '选择文件夹...' : 'Select folder...';
  String get selectOutputDirectoryTitle =>
      isZh ? '选择输出目录' : 'Select output directory';
  String get templateHint => 'frame_####';
  String templatePreview(String sample) => '-> $sample';
  String get skipExisting => isZh ? '跳过已有文件' : 'Skip existing';
  String get overwrite => isZh ? '覆盖' : 'Overwrite';
  String get autoVersion => isZh ? '自动版本化 (_v##)' : 'Auto-version (_v##)';
  String get diskWrites => isZh ? '写盘并发' : 'Disk Writes';
  String get bufferSize => isZh ? '缓冲大小' : 'Buffer Size';
  String writeBufferSummary(int writerThreads, int maxQueuedFrames) =>
      isZh ? '约 $writerThreads 个并行写入任务，最多缓冲 $maxQueuedFrames 帧。' : 'Runs about $writerThreads write jobs in parallel and keeps up to $maxQueuedFrames frames buffered.';
  String get dlssPreset => 'DLSS Preset';
  String get frameGen => 'Frame Gen';
  String get format => isZh ? '格式' : 'Format';
  String get pngQuality => 'PNG Quality';
  String get exrCompression => 'EXR Compression';
  String get compression => isZh ? '压缩' : 'Compression';
  String get outputDir => isZh ? '输出目录' : 'Output Dir';
  String get template => isZh ? '模板' : 'Template';
  String get onConflict => isZh ? '冲突处理' : 'On Conflict';
  String get exrUncompressed => isZh ? '无压缩' : 'Uncompressed';
  String get outputFormatPng16 => 'PNG (16-bit)';
  String get outputFormatExr => 'EXR (scene-linear)';
  String get outputFormatProRes422Hq => 'ProRes 422 HQ (.mov)';
  String get autoPreset => isZh ? '自动' : 'Auto';
  String get taskCancelled => isZh ? '任务已取消' : 'Task cancelled';
  String get connecting => 'DLSS Compositor';
  String fgMaxLabel(int value) => 'FG Max ${value}x';
  String get languageButtonSystemShort => isZh ? '系统' : 'SYS';
  String get languageButtonEnglishShort => 'EN';
  String get languageButtonChineseShort => '中';
}

extension AppStringsX on BuildContext {
  AppStrings get strings => AppStrings.of(this);
}
