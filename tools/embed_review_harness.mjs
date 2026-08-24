#!/usr/bin/env node
// embed_review_harness.mjs — turn the standalone reviewer and its asset folder
// into one shareable HTML file.
//
// Upstream: build_review_harness.mjs output plus optional lossless WebP
// sidecars. Downstream: a browser-openable file with no external asset reads.
// The image object is written property-by-property so large real-world review
// pools do not hit Node's maximum string length while being serialized.
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
const encodedByHash = new Map();
const webpRoot = process.env.TFORGE_REVIEW_WEBP_ROOT
  ? path.resolve(process.env.TFORGE_REVIEW_WEBP_ROOT)
  : null;

// Prepare only small path/hash records in memory. The encoded image payloads
// are read one at a time while writeEmbeddedAssetObject streams the final JSON.
const preparedAssets = externalAssets.map(asset => {
  if (!asset?.name || !asset?.src) throw new Error('Invalid external asset entry');
  const source = path.resolve(inputDir, asset.src);
  if (!fs.existsSync(source)) throw new Error(`Missing review asset: ${source}`);
  const webpSource = webpRoot ? path.join(webpRoot, asset.name.replace(/\.png$/i, '.webp')) : null;
  const encodedSource = webpSource && fs.existsSync(webpSource) ? webpSource : source;
  const bytes = fs.readFileSync(encodedSource);
  const hash = createHash('sha1').update(bytes).digest('hex');
  const mime = encodedSource.endsWith('.webp') ? 'image/webp' : 'image/png';
  if (!encodedByHash.has(hash)) encodedByHash.set(hash, {source: encodedSource, mime});
  return {name: asset.name, source: encodedSource, mime, hash};
});
const mimeCounts = new Map();
for (const asset of preparedAssets) mimeCounts.set(asset.mime, (mimeCounts.get(asset.mime) ?? 0) + 1);
const formatSummary = [...mimeCounts.entries()].map(([mime, count]) => `${mime}=${count}`).join(', ');

const embeddedStart = 'const embeddedAssets = Object.freeze(';
const embeddedDataStart = 'const embeddedAssetData = Object.freeze(';
const embeddedIndex = html.indexOf(embeddedStart);
if (embeddedIndex < 0) throw new Error('Could not find the embedded asset manifest');
const embeddedEnd = html.indexOf(');', embeddedIndex + embeddedStart.length);
if (embeddedEnd < 0) throw new Error('Embedded asset manifest is truncated');

// writeEmbeddedAssetObject: stream each encoded property directly to the
// output file. Upstream is the prepared asset list; downstream is the browser's
// embeddedAssets lookup. This is intentionally not JSON.stringify on the full
// object because a large corpus can exceed Node's single-string limit.
function writeEmbeddedAssetObject(fd, assets) {
  // The hash-keyed data object stores identical image bytes once. The
  // name-keyed map below preserves the browser-facing asset lookup contract
  // while aliases point at the shared data object instead of repeating a
  // multi-megabyte data URL.
  const uniqueAssets = [...new Map(assets.map(asset => [asset.hash, asset])).values()];
  fs.writeSync(fd, `${embeddedDataStart}{`);
  uniqueAssets.forEach((asset, index) => {
    if (index) fs.writeSync(fd, ',');
    const bytes = fs.readFileSync(asset.source);
    const encoded = `data:${asset.mime};base64,${bytes.toString('base64')}`;
    fs.writeSync(fd, JSON.stringify(asset.hash));
    fs.writeSync(fd, ':');
    fs.writeSync(fd, JSON.stringify(encoded));
  });
  fs.writeSync(fd, '});\n');
  fs.writeSync(fd, `${embeddedStart}{`);
  assets.forEach((asset, index) => {
    if (index) fs.writeSync(fd, ',');
    fs.writeSync(fd, JSON.stringify(asset.name));
    fs.writeSync(fd, `:embeddedAssetData[${JSON.stringify(asset.hash)}]`);
  });
  fs.writeSync(fd, '});');
}

const professionalStyles = '';
const transformSegment = segment => segment
  .replace('</head>', `${professionalStyles}\n</head>`)
  .replace(
    /Standalone review page · asset folder: [^·<]+ · benchmark source images are not modified/,
    'Standalone review page · all image assets embedded in this file · benchmark source images are not modified',
  )
  .replace('<title>Temporal Forge still review</title>', '<title>Temporal Forge · Still Review</title>');

// The HTML shell is small enough to keep in memory. Encoded payloads are
// streamed between shell segments, and a temporary sibling prevents a failed
// build from leaving a truncated destination artifact.
const prefix = transformSegment(html.slice(0, embeddedIndex));
let suffix = transformSegment(html.slice(embeddedEnd + 2));
const newExternalIndex = suffix.indexOf(externalStart);
if (newExternalIndex < 0) throw new Error('Could not find the external asset manifest in the suffix');
const newExternalEnd = suffix.indexOf(');', newExternalIndex + externalStart.length);
if (newExternalEnd < 0) throw new Error('External asset manifest in suffix is truncated');
suffix = suffix.slice(0, newExternalIndex)
  + `${externalStart}[]);`
  + suffix.slice(newExternalEnd + 2);

const temporaryOutput = `${output}.tmp-${process.pid}`;
const maxMiB = Number(process.env.TFORGE_REVIEW_MAX_MIB ?? '512');
if (!Number.isFinite(maxMiB) || maxMiB <= 0) {
  throw new Error('TFORGE_REVIEW_MAX_MIB must be a positive number');
}
const fd = fs.openSync(temporaryOutput, 'w');
try {
  fs.writeSync(fd, prefix);
  writeEmbeddedAssetObject(fd, preparedAssets);
  fs.writeSync(fd, suffix);
} finally {
  fs.closeSync(fd);
}
const temporarySizeMiB = fs.statSync(temporaryOutput).size / 1024 / 1024;
if (temporarySizeMiB > maxMiB) {
  fs.unlinkSync(temporaryOutput);
  throw new Error(
    `embedded review HTML is ${temporarySizeMiB.toFixed(1)} MiB, above `
    + `TFORGE_REVIEW_MAX_MIB=${maxMiB}`,
  );
}
fs.renameSync(temporaryOutput, output);

const sizeMiB = temporarySizeMiB.toFixed(1);
console.log(`wrote ${output} as a self-contained ${sizeMiB} MiB HTML file with ${preparedAssets.length} embedded assets (${encodedByHash.size} unique source payloads; ${formatSummary})`);
