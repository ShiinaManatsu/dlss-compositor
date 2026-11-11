import {transform} from 'lightningcss';
import type {Plugin} from 'vite';

/** Dev macro CSS needs the same selector lowering as the production CSS pipeline.
 * Baseline is configurable: select it from the actual browser/WebView support policy.
 * In S2 1.7.1, unlowered multi-argument :lang() was dropped by the tested embedded browser.
 */
export function s2DevCssCompatibility(targets = {chrome: 100 << 16}): Plugin {
  return {
    name: 's2-dev-css-compatibility', enforce: 'pre', apply: 'serve',
    transform(code, id) {
      const filename = id.split('?')[0];
      if (!filename.endsWith('.css') || !filename.includes('macro-')) return;
      const result = transform({filename, code: new TextEncoder().encode(code), minify: true, targets});
      return {code: new TextDecoder().decode(result.code), map: null};
    }
  };
}
