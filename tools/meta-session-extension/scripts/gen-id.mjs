import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const der = readFileSync(process.argv[2]);
const hash = createHash("sha256").update(der).digest();
const first16 = hash.subarray(0, 16);
let id = "";
for (const byte of first16) {
  id += ((byte >>> 4) & 0xf).toString(16);
  id += (byte & 0xf).toString(16);
}
const mapped = id
  .split("")
  .map(c => String.fromCharCode(97 + parseInt(c, 16)))
  .join("");
console.log(mapped);
