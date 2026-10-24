import { readFileSync, writeFileSync } from 'node:fs';
import { Resvg } from '@resvg/resvg-js';

const [, , inPath, outPath, widthArg] = process.argv;
const svg = readFileSync(inPath, 'utf8');
const opts = { background: 'white' };
if (widthArg) opts.fitTo = { mode: 'width', value: parseInt(widthArg, 10) };

const resvg = new Resvg(svg, opts);
const png = resvg.render();
writeFileSync(outPath, png.asPng());
console.log(`OK ${outPath} ${png.width}x${png.height}`);
