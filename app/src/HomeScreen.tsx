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
import FileVideo from '@react-spectrum/s2/illustrations/gradient/generic1/FileVideo';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import type {RecentProject} from './dcp';

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

export function HomeScreen({
  recents,
  onOpen,
  onPickOpen,
  onNew
}: {
  recents: RecentProject[];
  onOpen: (dcpPath: string) => void;
  onPickOpen: () => void;
  onNew: () => void;
}) {
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
    <div className="home-body">
      <aside className="home-nav">
        <span className={style({font: 'title-xs'})}>最近</span>
      </aside>
      <section className="home-grid">
        <CardView
          aria-label="最近工程"
          items={recents.map((item) => ({...item, id: item.path}))}
          layout="grid"
          size="S"
          onAction={(key) => onOpen(String(key))}
          styles={style({width: 'full', height: 'full'})}
          renderEmptyState={() => (
            <IllustratedMessage size="L">
              <FileVideo />
              <Heading>没有最近工程</Heading>
              <ButtonGroup>
                <Button variant="secondary" onPress={onPickOpen}>
                  打开
                </Button>
                <Button variant="accent" onPress={onNew}>
                  新建
                </Button>
              </ButtonGroup>
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
      </section>
    </div>
  );
}
