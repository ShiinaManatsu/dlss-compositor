import {useEffect, useId, useRef, useState, type ReactNode} from 'react';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};
import {ActionButton, Text} from '@react-spectrum/s2/ActionButton';
import {Dialog, DialogTrigger, Heading, Content} from '@react-spectrum/s2/Dialog';
import Properties from '@react-spectrum/s2/icons/Properties';

const surface = style({backgroundColor: 'layer-1', borderRadius: 'xl'});
const MIN = 280;
const DEFAULT = 320;

/** Adjacent panels share one bounded splitter; media always gets the remainder. */
export function Workbench({name, children}: {name: string; children: ReactNode}) {
  const root = useRef<HTMLDivElement>(null);
  const id = useId();
  const key = `dc-panel-width-${name}`;
  const [preferred, setPreferred] = useState(() => {
    try {
      const value = Number(localStorage.getItem(key));
      return Number.isFinite(value) && value >= MIN ? Math.min(440, value) : DEFAULT;
    } catch { return DEFAULT; }
  });
  const [maximum, setMaximum] = useState(440);
  const [compact, setCompact] = useState(false);
  const [dialogOpen, setDialogOpen] = useState(false);
  const [gap, setGap] = useState(16);
  const width = Math.min(preferred, maximum);
  const drag = useRef<{x: number; width: number} | null>(null);
  useEffect(() => {
    const observer = new ResizeObserver(() => {
      const el = root.current;
      if (!el) return;
      const gutter = parseFloat(getComputedStyle(el).getPropertyValue('--shell-gap')) || 16;
      setGap(gutter);
      setMaximum(Math.max(MIN, Math.min(440, el.clientWidth - 360 - gutter)));
      const isCompact = el.clientWidth < 920 || el.clientHeight < 420;
      setCompact(isCompact);
      if (!isCompact) setDialogOpen(false);
    });
    if (root.current) observer.observe(root.current);
    return () => observer.disconnect();
  }, []);
  function save(value: number) {
    const next = Math.max(MIN, Math.min(maximum, value));
    setPreferred(next);
    try { localStorage.setItem(key, String(next)); } catch { /* unavailable storage */ }
  }
  const panels = Array.isArray(children) ? children : [children];
  return (
    <div className={`export-body${compact ? ' is-compact' : ''}`} ref={root}
      style={compact ? undefined : {gridTemplateColumns: `${width}px ${gap}px minmax(0, 1fr)`}}>
      {compact ? <div className="workbench-tools">
        <DialogTrigger isOpen={dialogOpen} onOpenChange={setDialogOpen}>
          <ActionButton><Properties /><Text>参数</Text></ActionButton>
          <Dialog isDismissible><Heading slot="title">{name === 'export' ? '导出参数' : 'NR / FG 参数'}</Heading>
            <Content><div className="workbench-dialog">{panels[0]}</div></Content>
          </Dialog>
        </DialogTrigger>
      </div> : <>
      <div id={id} className={`workbench-inspector ${surface}`}>{panels[0]}</div>
      <div role="separator" tabIndex={0} aria-label="参数面板宽度（方向键调整，Home 重置）"
        aria-orientation="vertical" aria-controls={id} aria-valuemin={MIN} aria-valuemax={maximum}
        aria-valuenow={Math.round(width)} className="panel-splitter"
        onDoubleClick={() => save(DEFAULT)}
        onKeyDown={(event) => {
          const delta = event.shiftKey ? 32 : 16;
          if (event.key === 'ArrowLeft' || event.key === 'ArrowRight' || event.key === 'Home') {
            event.preventDefault();
            save(event.key === 'Home' ? DEFAULT : width + (event.key === 'ArrowLeft' ? -delta : delta));
          }
        }}
        onPointerDown={(event) => {
          if (event.button !== 0) return;
          drag.current = {x: event.clientX, width};
          event.currentTarget.setPointerCapture(event.pointerId);
          event.preventDefault();
        }}
        onPointerMove={(event) => {
          if (drag.current) setPreferred(Math.max(MIN, Math.min(maximum, drag.current.width + event.clientX - drag.current.x)));
        }}
        onPointerUp={(event) => {
          if (!drag.current) return;
          save(drag.current.width + event.clientX - drag.current.x);
          drag.current = null;
          event.currentTarget.releasePointerCapture(event.pointerId);
        }}
        onPointerCancel={() => { drag.current = null; }}
        onLostPointerCapture={() => { drag.current = null; }} />
      </>}
      <div className={`workbench-viewer ${surface}`}>{panels[1]}</div>
    </div>
  );
}
