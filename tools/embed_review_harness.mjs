#!/usr/bin/env node
// embed_review_harness.mjs — turn the standalone reviewer and its asset folder
// into one shareable HTML file.
//
// Upstream: build_review_harness.mjs output plus optional lossless WebP
// sidecars. Downstream: a browser-openable file with no external asset reads.
// Deduplication is by encoded-image hash, so identical real assets do not
// multiply the distributable size or alter what the reviewer displays.
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';

const input = path.resolve(process.argv[2] ?? 'temporal-forge-frame55-review-standalone.html');
const output = path.resolve(process.argv[3] ?? 'temporal-forge-frame55-review-single-file.html');

if (!fs.existsSync(input)) throw new Error(`Review HTML does not exist: ${input}`);

const html = fs.readFileSync(input, 'utf8');
const externalStart = 'const externalAssets = Object.freeze(';
const externalIndex = html.indexOf(externalStart);
if (externalIndex < 0) throw new Error('Could not find the external asset manifest');
const externalEnd = html.indexOf(');', externalIndex + externalStart.length);
if (externalEnd < 0) throw new Error('External asset manifest is truncated');

const externalSource = html.slice(externalIndex + externalStart.length, externalEnd);
const externalAssets = JSON.parse(externalSource);
const inputDir = path.dirname(input);
const embeddedAssets = {};
const encodedByHash = new Map();
const webpRoot = process.env.TFORGE_REVIEW_WEBP_ROOT
  ? path.resolve(process.env.TFORGE_REVIEW_WEBP_ROOT)
  : null;

for (const asset of externalAssets) {
  if (!asset?.name || !asset?.src) throw new Error('Invalid external asset entry');
  const source = path.resolve(inputDir, asset.src);
  if (!fs.existsSync(source)) throw new Error(`Missing review asset: ${source}`);
  const webpSource = webpRoot ? path.join(webpRoot, asset.name.replace(/\.png$/i, '.webp')) : null;
  const encodedSource = webpSource && fs.existsSync(webpSource) ? webpSource : source;
  const bytes = fs.readFileSync(encodedSource);
  const hash = createHash('sha1').update(bytes).digest('hex');
  let encoded = encodedByHash.get(hash);
  if (!encoded) {
    const mime = encodedSource.endsWith('.webp') ? 'image/webp' : 'image/png';
    encoded = `data:${mime};base64,${bytes.toString('base64')}`;
    encodedByHash.set(hash, encoded);
  }
  embeddedAssets[asset.name] = encoded;
}

const embeddedStart = 'const embeddedAssets = Object.freeze(';
const embeddedIndex = html.indexOf(embeddedStart);
if (embeddedIndex < 0) throw new Error('Could not find the embedded asset manifest');
const embeddedEnd = html.indexOf(');', embeddedIndex + embeddedStart.length);
if (embeddedEnd < 0) throw new Error('Embedded asset manifest is truncated');

const professionalStyles = '';

let rendered = html.slice(0, embeddedIndex)
  + `${embeddedStart}${JSON.stringify(embeddedAssets)});`
  + html.slice(embeddedEnd + 2);

const newExternalIndex = rendered.indexOf(externalStart);
const newExternalEnd = rendered.indexOf(');', newExternalIndex + externalStart.length);
rendered = rendered.slice(0, newExternalIndex)
  + `${externalStart}[]);`
  + rendered.slice(newExternalEnd + 2);

rendered = rendered.replace('</head>', `${professionalStyles}\n</head>`);
rendered = rendered.replace(
  /Standalone review page · asset folder: [^·<]+ · benchmark source images are not modified/,
  'Standalone review page · all image assets embedded in this file · benchmark source images are not modified',
);
rendered = rendered.replace('<title>Temporal Forge still review</title>', '<title>Temporal Forge · Still Review</title>');

fs.writeFileSync(output, rendered);
const sizeMiB = (fs.statSync(output).size / 1024 / 1024).toFixed(1);
console.log(`wrote ${output} as a self-contained ${sizeMiB} MiB HTML file with ${Object.keys(embeddedAssets).length} embedded assets (${encodedByHash.size} unique images, lossless WebP)`);
