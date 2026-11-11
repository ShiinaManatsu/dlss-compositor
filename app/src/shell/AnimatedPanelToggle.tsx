import {createContext, useContext, useState} from 'react';
import {useHover} from 'react-aria/useHover';
import {ActionButton} from '@react-spectrum/s2/ActionButton';
import {createIcon} from '@react-spectrum/s2/Icon';
import {Tooltip, TooltipTrigger} from '@react-spectrum/s2/Tooltip';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

const PanelState = createContext({isExpanded: false, isPreviewExpanded: false});
// SVG artwork adapted from Adobe react-spectrum Sidebar.tsx. Copyright Adobe.
// Apache-2.0: https://www.apache.org/licenses/LICENSE-2.0
// https://github.com/adobe/react-spectrum/blob/main/packages/dev/s2-docs/pages/s2/home/app/Sidebar.tsx
const PanelIcon = createIcon(props => {
  const state = useContext(PanelState);
  return <svg viewBox="0 0 20 20" fill="var(--iconPrimary)" {...props}>
    <path d="M15.75 18H4.25C3.00977 18 2 16.9907 2 15.75V4.25C2 3.00928 3.00977 2 4.25 2H15.75C16.9902 2 18 3.00928 18 4.25V15.75C18 16.9907 16.9902 18 15.75 18ZM4.25 3.5C3.83691 3.5 3.5 3.83643 3.5 4.25V15.75C3.5 16.1636 3.83691 16.5 4.25 16.5H15.75C16.1631 16.5 16.5 16.1636 16.5 15.75V4.25C16.5 3.83643 16.1631 3.5 15.75 3.5H4.25Z" />
    <rect data-panel-icon-bar x={5} y={5} rx={0.5} height={10} className={style({
      width: {default: '[1.5px]', isPreviewExpanded: '[5px]',
        '@media (prefers-reduced-motion: reduce)': {default: '[1.5px]', isExpanded: '[5px]'}},
      transition: '[width]', transitionDuration: {default: 300, '@media (prefers-reduced-motion: reduce)': 0}
    })(state)} />
  </svg>;
});

export function AnimatedPanelToggle({isExpanded, onExpandedChange, controls,
  expandLabel = 'Expand navigation', collapseLabel = 'Collapse navigation'}: {
  isExpanded: boolean; onExpandedChange: (value: boolean) => void; controls?: string;
  expandLabel?: string; collapseLabel?: string;
}) {
  const [preview, setPreview] = useState(false);
  const {hoverProps} = useHover({onHoverChange: setPreview});
  const label = isExpanded ? collapseLabel : expandLabel;
  return <span {...hoverProps} data-panel-toggle className={style({display: 'inline-flex'})}>
    <PanelState.Provider value={{isExpanded, isPreviewExpanded: preview ? !isExpanded : isExpanded}}>
      <TooltipTrigger><ActionButton isQuiet aria-label={label} aria-expanded={isExpanded} aria-controls={controls}
        onPress={() => {setPreview(false); onExpandedChange(!isExpanded);}}><PanelIcon /></ActionButton>
        <Tooltip>{label}</Tooltip></TooltipTrigger>
    </PanelState.Provider>
  </span>;
}
