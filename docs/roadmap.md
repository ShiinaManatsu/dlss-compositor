# Roadmap Notes

Working notes for future DLSS Compositor milestones. Keep this as intent and design context, not a replacement for issues or implementation docs.

## Next backend (before GUI)

See DESIGN.md §9 (2026-08-28). Do these in order, then start the GUI:

1. Preview JPEG readback (PreviewPass exists; RPC encode path is still a stub).
2. DLL loading: 两个版本都锁定 `resources/` 的 310.7.129.0（nvngx_dlss/dlssd/dlssg），拷到 cli.exe 旁；NGX core 仍来自驱动，不打包 `_nvngx.dll`。
3. 打包：electron-builder（`app/`，`npm run dist`）产出 NSIS 安装包（`.dcp` 关联）+ 便携 zip；`app/scripts/pack-backend.mjs` 组装 backend（cli + *.spv + 锁定 DLL + pipeline 脚本）。旧的 `scripts/package_release.ps1`（Flutter 时代）已移除。

After that, in descending priority: last-frame dirty background (full sequence) → SR dual-pass → HDR ProRes.

## Installer

- 2026-08-30: electron-builder NSIS 安装包 + 便携 zip（见上「打包」条目）。
- 安装包不带 NGX core（`_nvngx.dll` 仍来自驱动）；三个 feature DLL（nvngx_dlss/dlssd/dlssg，310.7.129.0）随包发布在 `resources/backend/cli/`。
- `.dcp` 文件关联已注册（per-user）；运行期单实例，二次双击 .dcp 转发给已开窗口。

## Render-To-Video Scratch Storage

ImDisk Stage 3 is in (`pipeline/run.py` attach existing `R:`, FileWatcher consume/delete, ProRes to working disk). Do not unmount the member disk. WinFsp / auto-create can wait until the GUI owns disk lifecycle.
