import {Content, Divider, InlineAlert, LabeledValue, Link, Picker, PickerItem, StatusLight, Switch} from '@react-spectrum/s2';
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
    <div className="page-settings">
      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>外观</h3>
        <Picker
          label="颜色方案"
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
      </section>

      <Divider size="S" />

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>环境</h3>
        <LabeledValue label="Blender" value={probe?.blender.path || '未检测到'} />
        <LabeledValue label="CLI" value={probe?.cli.path || '未检测到'} />
        <LabeledValue label="ImDisk" value={imdiskOk ? (probe?.imdisk.path ?? '已安装') : userErrorMessage(probe?.imdisk.message ?? '请安装 ImDisk Toolkit')} />
        <div className="settings-row">
          <LabeledValue
            label="内存盘"
            value={
              ramOk
                ? `${probe?.ramdisk.letter}: ${probe?.ramdisk.label || 'DLSSRAM'}`
                : '未挂载'
            }
          />
          {ramOk ? (
            <StatusLight size="S" variant="positive">就绪</StatusLight>
          ) : null}
        </div>
        <InlineAlert variant="informative" styles={style({width: 'full'})}>
          <Content>
            渲染帧的高速中转缓冲：任务开始自动创建、软件退出自动卸载。依赖{' '}
            <Link href="https://github.com/LTRData/ImDisk">ImDisk</Link>。
          </Content>
        </InlineAlert>
        <Switch size="S" isSelected={app.elevatePref} onChange={app.setElevatePref}>
          以管理员身份运行
        </Switch>
        <Switch size="S" isSelected={app.cliLog} onChange={app.setCliLog}>
          CLI 调试日志
        </Switch>
      </section>

      <Divider size="S" />

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>DLSS</h3>
        <LabeledValue label="SR / RR / FG DLL" value="310.7.129.0（锁定）" />
      </section>

      <Divider size="S" />

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>关于</h3>
        <LabeledValue label="DLSS Compositor" value={`v${getAppVersion()}`} />
      </section>
    </div>
  );
}
