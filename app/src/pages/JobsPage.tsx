import {useEffect, useState} from 'react';
import {Content, Heading, IllustratedMessage} from '@react-spectrum/s2/IllustratedMessage';
import {ListView, ListViewItem, Text} from '@react-spectrum/s2/ListView';
import {ActionMenu, MenuItem} from '@react-spectrum/s2/ActionMenu';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import Copy from '@react-spectrum/s2/icons/Copy';
import Filmstrip from '@react-spectrum/s2/icons/Filmstrip';
import Video from '@react-spectrum/s2/icons/Video';
import AlertTriangle from '@react-spectrum/s2/icons/AlertTriangle';
import FileVideo from '@react-spectrum/s2/illustrations/gradient/generic1/FileVideo';
import {useApp, padFrame, statusLabel} from '../appState';
import {
  extensionForFormat,
  formatLabel,
  isSequenceFormat,
  joinPath,
  sequenceBase,
  type JobRecord
} from '../dcp';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

type OutputRow = {
  key: string;
  kind: 'sequence' | 'video';
  title: string;
  description: string;
  status: JobRecord['status'] | 'running';
  outputDir: string;
  file?: string;
};

function recordKey(record: JobRecord) {
  return `${record.finishedAt}-${record.template}`;
}

function sequenceFrameRange(record: JobRecord) {
  return `${padFrame(record.frameStart)}–${padFrame(record.frameEnd)}`;
}

export function JobsPage() {
  const app = useApp();
  const [seqCounts, setSeqCounts] = useState<Record<string, number>>({});

  const rows: OutputRow[] = [];

  const active = app.jobs.find((job) => job.status === 'running');
  if (active) {
    rows.push({
      key: `active-${active.id}`,
      kind: isSequenceFormat(active.outputFormat) ? 'sequence' : 'video',
      title: `${active.name} ${padFrame(active.frameStart)}–${padFrame(active.frameEnd)}`,
      description: `${active.mode.toUpperCase()} · ${formatLabel(active.outputFormat)} · 导出中 · ${active.outputDir}`,
      status: 'running',
      outputDir: active.outputDir
    });
  }

  for (const [index, record] of app.history.entries()) {
    const sequence = isSequenceFormat(record.format);
    const base = sequence ? sequenceBase(record.template) : record.template;
    const ext = extensionForFormat(record.format);
    const title = sequence ? `${base}${sequenceFrameRange(record)}${ext}` : record.template;
    const count = seqCounts[recordKey(record)];
    const statusText =
      record.status === 'done'
        ? ''
        : record.status === 'cancelled'
          ? ' · 已停止'
          : ` · ${statusLabel(record.status)}`;
    const countText =
      sequence && count != null ? ` · ${count} 帧` : sequence && count === 0 ? ' · 未找到文件' : '';
    rows.push({
      key: `record-${index}-${record.finishedAt}`,
      kind: sequence ? 'sequence' : 'video',
      title,
      description: `${formatLabel(record.format)}${
        record.width ? ` · ${record.width}×${record.height}` : ''
      }${countText}${statusText} · ${record.outputDir}`,
      status: record.status,
      outputDir: record.outputDir,
      file: sequence ? undefined : joinPath(record.outputDir, record.template)
    });
  }

  // Scan sequence directories once per record set to show real frame counts.
  useEffect(() => {
    let cancelled = false;
    void Promise.all(
      app.history
        .filter((record) => isSequenceFormat(record.format))
        .map(async (record) => {
          const entries = await window.dlss.listDir(record.outputDir);
          if (!entries) return [recordKey(record), 0] as const;
          const base = sequenceBase(record.template);
          const ext = extensionForFormat(record.format).toLowerCase();
          const count = entries.filter(
            (entry) =>
              !entry.isDir && entry.name.toLowerCase().endsWith(ext) && entry.name.startsWith(base)
          ).length;
          return [recordKey(record), count] as const;
        })
    ).then((pairs) => {
      if (!cancelled) setSeqCounts(Object.fromEntries(pairs));
    });
    return () => {
      cancelled = true;
    };
  }, [app.history]);

  return (
    <div className={`page-jobs ${style({backgroundColor: 'layer-1', borderRadius: 'xl'})}`}>
      <ListView
        aria-label="导出产物"
        selectionMode="none"
        isQuiet
        styles={style({width: 'full', height: 'full'})}
        renderEmptyState={() => (
          <IllustratedMessage size="L">
            <FileVideo />
            <Heading>还没有导出产物</Heading>
            <Content>导出后，这里会列出项目生成的序列与视频。</Content>
          </IllustratedMessage>
        )}>
        {rows.map((row) => (
          <ListViewItem key={row.key} id={row.key} textValue={row.title}>
            {row.status === 'error' ? (
              <AlertTriangle />
            ) : row.kind === 'sequence' ? (
              <Filmstrip />
            ) : (
              <Video />
            )}
            <Text>{row.title}</Text>
            <Text slot="description">{row.description}</Text>
            <ActionMenu align="end">
              <MenuItem
                textValue="打开文件夹"
                onAction={() => void window.dlss.openPath(row.outputDir)}>
                <FolderOpen />
                <Text slot="label">打开文件夹</Text>
              </MenuItem>
              {row.file ? (
                <MenuItem textValue="打开文件" onAction={() => void window.dlss.openPath(row.file ?? '')}>
                  <Video />
                  <Text slot="label">打开文件</Text>
                </MenuItem>
              ) : null}
              <MenuItem
                textValue="复制路径"
                onAction={() => {
                  void navigator.clipboard.writeText(row.file ?? row.outputDir);
                }}>
                <Copy />
                <Text slot="label">复制路径</Text>
              </MenuItem>
            </ActionMenu>
          </ListViewItem>
        ))}
      </ListView>
    </div>
  );
}
