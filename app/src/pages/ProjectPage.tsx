import {useEffect, useState} from 'react';
import {
  Button,
  ButtonGroup,
  CardView,
  AssetCard,
  CardPreview,
  Content,
  Heading,
  IllustratedMessage,
  Image,
  Text
} from '@react-spectrum/s2';
import FolderOpen from '@react-spectrum/s2/icons/FolderOpen';
import Add from '@react-spectrum/s2/icons/Add';
import FileVideo from '@react-spectrum/s2/illustrations/gradient/generic1/FileVideo';
import {useApp} from '../appState';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import type {RecentProject} from '../dcp';

function formatStamp(mtime: number) {
  if (!mtime) return '';
  return new Date(mtime).toLocaleString('zh-CN', {
    year: 'numeric',
    month: 'numeric',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit'
  });
}

export function ProjectPage() {
  const {recents, openDcp, pickOpenDcp, newProject} = useApp();
  const [previews, setPreviews] = useState<Record<string, string | null>>({});

  useEffect(() => {
    let cancelled = false;
    void Promise.all(
      recents.map(async (item) => {
        if (!item.preview) return [item.path, null] as const;
        const url = await window.dlss.previewDataUrl(item.preview);
        return [item.path, url] as const;
      })
    ).then((pairs) => {
      if (!cancelled) setPreviews(Object.fromEntries(pairs));
    });
    return () => {
      cancelled = true;
    };
  }, [recents]);

  return (
    <div className="page-project">
      <div className="page-header">
        <Heading level={2}>项目</Heading>
        <ButtonGroup>
          <Button variant="secondary" onPress={() => void pickOpenDcp()}>
            <FolderOpen />
            <Text>打开</Text>
          </Button>
          <Button variant="accent" onPress={() => void newProject()}>
            <Add />
            <Text>新建项目</Text>
          </Button>
        </ButtonGroup>
      </div>
      <div className="project-grid">
        <CardView
          aria-label="最近工程"
          items={recents.map((item) => ({...item, id: item.path}))}
          layout="grid"
          size="M"
          onAction={(key) => void openDcp(String(key))}
          styles={style({width: 'full', height: 'full'})}
          renderEmptyState={() => (
            <IllustratedMessage size="L">
              <FileVideo />
              <Heading>没有最近工程</Heading>
              <Content>打开一个 .dcp 工程，或从 .blend 新建。</Content>
            </IllustratedMessage>
          )}>
          {(item: RecentProject) => (
            <AssetCard id={item.path} textValue={item.name} size="S">
              <CardPreview>
                {previews[item.path] ? (
                  <Image src={previews[item.path] ?? ''} alt="" />
                ) : (
                  <FileVideo />
                )}
              </CardPreview>
              <Content>
                <Text slot="title">{item.name}</Text>
                <Text slot="description">
                  {(item.mode || 'dcp').toUpperCase()}
                  {item.mtime ? ` · ${formatStamp(item.mtime)}` : ''}
                </Text>
              </Content>
            </AssetCard>
          )}
        </CardView>
      </div>
    </div>
  );
}
