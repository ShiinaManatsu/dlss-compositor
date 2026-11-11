import type {Plugin} from 'vite';

// Product-owned dependency CSS policy for S2 1.6.0: retain Adobe Han first,
// add Windows sans fallback to simplified Chinese font-family declarations.
// Cover compiled controls and app macros equally without editing node_modules
// or overriding component selectors/weights/geometry. Recheck on S2 upgrades.
function fontPolicy(css: string) {
  return css.replace(/font-family\s*:[^;}]+/g, declaration => {
    if (!declaration.includes('adobe-clean-han-simplified-c') || declaration.includes('Microsoft YaHei')) return declaration;
    return declaration.replace(/['"]?SimSun['"]?/, '"Microsoft YaHei UI", "Microsoft YaHei", SimSun');
  });
}

export function s2WindowsFontPolicy(): Plugin {
  return {
    name: 's2-windows-font-policy', enforce: 'pre',
    transform(code, id) {
      if (id.split('?')[0].endsWith('.css')) return {code: fontPolicy(code), map: null};
    },
    generateBundle(_options, bundle) {
      for (const asset of Object.values(bundle)) {
        if (asset.type === 'asset' && asset.fileName.endsWith('.css') && typeof asset.source === 'string') {
          asset.source = fontPolicy(asset.source);
        }
      }
    }
  };
}
