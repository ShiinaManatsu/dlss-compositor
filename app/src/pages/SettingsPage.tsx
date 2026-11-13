import {Content, InlineAlert} from '@react-spectrum/s2/InlineAlert';
import {LabeledValue} from '@react-spectrum/s2/LabeledValue';
import {Link} from '@react-spectrum/s2/Link';
import {Picker, PickerItem} from '@react-spectrum/s2/Picker';
import {Switch} from '@react-spectrum/s2/Switch';
import {Button} from '@react-spectrum/s2/Button';
import {getAppVersion, useApp, type ColorSchemePref} from '../appState';
import {userErrorMessage} from '../errors';
import {style} from '@react-spectrum/s2/style' with {type: 'macro'};

const SCHEME_OPTIONS = [
  {id: 'dark', label: '深色'},
  {id: 'light', label: '浅色'},
  {id: 'system', label: '跟随系统'}
] as const;

export function SettingsPage() {
  const app = useApp();
  const probe = app.probe;
  const ramOk = probe?.ramdisk.ok;
  const imdiskOk = probe?.imdisk.ok;

  return (
    <div className={`page-settings ${style({backgroundColor: 'layer-1', borderRadius: 'xl'})}`}>
      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>外观</h3>
        <Picker
          label="主题"
          size="S"
          styles={style({width: 200})}
          items={SCHEME_OPTIONS}
          value={app.colorScheme}
          onChange={(key) => app.setColorScheme(String(key) as ColorSchemePref)}>
          {(item) => (
            <PickerItem key={item.id} id={item.id}>
              {item.label}
            </PickerItem>
          )}
        </Picker>
        <Button size="S" variant="secondary" styles={style({alignSelf: 'start'})} onPress={() => {
          try {
            localStorage.removeItem('dc-panel-width-export');
            localStorage.removeItem('dc-panel-width-nrtool');
          } catch { /* unavailable storage */ }
        }}>重置面板宽度</Button>
      </section>

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>环境</h3>
        <LabeledValue label="Blender" value={probe?.blender.path || '未检测到'} />
        <LabeledValue label="CLI" value={probe?.cli.path || '未检测到'} />
        <LabeledValue label="ImDisk" value={imdiskOk ? (probe?.imdisk.path ?? '已安装') : userErrorMessage(probe?.imdisk.message ?? '请安装 ImDisk Toolkit')} />
        <LabeledValue
          label="内存盘"
          value={ramOk ? `${probe?.ramdisk.letter}: ${probe?.ramdisk.label || 'DLSSRAM'}` : '未挂载'}
        />
        <InlineAlert variant="informative" styles={style({width: 'full'})}>
          <Content>
            通过{' '}
            <Link href="https://github.com/LTRData/ImDisk" target="_blank" rel="noreferrer">ImDisk</Link>
            {' '}自动挂载内存盘，退出时卸载。
          </Content>
        </InlineAlert>
        <Switch size="S" isSelected={app.cliLog} onChange={app.setCliLog} isDisabled={app.busy}>
          CLI 调试日志
        </Switch>
      </section>

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>DLSS</h3>
        <LabeledValue label="SR / RR / FG DLL" value="310.9.16.0" />
      </section>

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>关于</h3>
        <LabeledValue label="DLSS Compositor" value={`v${getAppVersion()}`} />
      </section>
    </div>
  );
}
