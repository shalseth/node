import { parentPort, workerData } from 'node:worker_threads';
import { total } from './compute.ts';
import type { Row } from './types.ts';
const rows: Row[] = workerData.rows;
parentPort!.postMessage(total(rows));
