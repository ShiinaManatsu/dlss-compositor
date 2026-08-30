import {Divider, LabeledValue, Picker, PickerItem, StatusLight} from '@react-spectrum/s2';
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
                : userErrorMessage(probe?.ramdisk.message ?? '未找到 DLSSRAM')
            }
          />
          <StatusLight size="S" variant={ramOk ? 'positive' : 'negative'}>
            {ramOk ? '就绪' : '未挂载'}
          </StatusLight>
        </div>
        <span className={style({font: 'detail-sm', color: 'gray-600'})}>
          输入缓冲二选一：勾选「使用内存盘」时，安装版自动创建/卸载 DLSSRAM（需管理员），开发版手动挂载；不勾选则使用导出页的「缓存路径」（本地或网络目录均可）。
        </span>
      </section>

      <Divider size="S" />

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>DLSS</h3>
        <LabeledValue label="SR / RR / FG DLL" value="310.7.129.0（锁定）" />
        <span className={style({font: 'detail-sm', color: 'gray-600'})}>
          Debug / Release 都用 resources/ 里锁死的 310.7.129.0，拷到 cli.exe 旁。NGX core 由驱动提供。不需要在 GUI 里切 debug 后端。
        </span>
      </section>

      <Divider size="S" />

      <section className="settings-group">
        <h3 className={style({font: 'heading-sm'})}>关于</h3>
        <LabeledValue label="DLSS Compositor" value={`v${getAppVersion()}`} />
      </section>
    </div>
  );
}
