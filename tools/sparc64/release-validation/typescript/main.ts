import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer } from 'node:http';
import { Worker } from 'node:worker_threads';
import { total } from './compute.ts';
import type { Row } from './types.ts';
assert.equal(process.features.typescript, 'strip');
const count: number = 32;
function rows(salt: number): Row[] {
  return Array.from({ length: count }, (_, n) => ({ value: n + salt, weight: n + 1 }));
}
function expected(salt: number): number {
  return count * (count - 1) * (2 * count - 1) / 6 +
         (salt + 1) * count * (count - 1) / 2 + salt * count;
}
const workers: Worker[] = [];
const server = createServer(async (request, response) => {
  try {
    const chunks: Buffer[] = [];
    for await (const chunk of request) chunks.push(chunk);
    const input: Row[] = JSON.parse(Buffer.concat(chunks).toString());
    response.setHeader('content-type', 'application/json');
    response.end(JSON.stringify({ result: total(input) }));
  } catch (error) {
    response.statusCode = 500;
    response.end(String(error));
  }
});
try {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  assert.ok(address && typeof address === 'object');
  for (let base = 0; base < 24; base += 4) {
    await Promise.all(Array.from({ length: 4 }, async (_, index) => {
      const salt: number = base + index;
      const response = await fetch(`http://127.0.0.1:${address.port}/total`, {
        method: 'POST', body: JSON.stringify(rows(salt))
      });
      assert.equal(response.status, 200);
      const result: { result: number } = await response.json();
      assert.equal(result.result, expected(salt));
    }));
  }
  await Promise.all(Array.from({ length: 4 }, async (_, salt) => {
    const worker = new Worker(new URL('./worker.ts', import.meta.url), {
      workerData: { rows: rows(salt) }
    });
    workers.push(worker);
    const [result] = await once(worker, 'message');
    assert.equal(result, expected(salt));
  }));
  console.log('TYPESCRIPT_APPLICATION_OK ' + JSON.stringify({
    passed: true, httpRequests: 24, workers: 4, directTypeScript: true
  }));
} finally {
  await Promise.all(workers.map(worker => worker.terminate()));
  server.closeAllConnections();
  await new Promise<void>((resolve, reject) => server.close(error => error ? reject(error) : resolve()));
}
