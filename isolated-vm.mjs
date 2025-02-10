import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const module = require('./out/isolated_vm');
export default module;
