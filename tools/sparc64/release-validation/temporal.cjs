'use strict';

const assert = require('node:assert/strict');
const { Worker, isMainThread, parentPort } = require('node:worker_threads');

function schedule() {
  let checks = 0;
  function equal(actual, expected) {
    assert.equal(actual, expected);
    checks++;
  }
  const zone = 'Europe/Oslo';
  const spring = Temporal.ZonedDateTime.from('2026-03-28T12:00[Europe/Oslo]');
  const autumn = Temporal.ZonedDateTime.from('2026-10-24T12:00[Europe/Oslo]');
  equal(spring.add({ days: 1 }).hour, 12);
  equal(spring.add({ days: 1 }).epochNanoseconds - spring.epochNanoseconds,
        23n * 3600n * 1000000000n);
  equal(spring.add({ hours: 24 }).hour, 13);
  equal(autumn.add({ days: 1 }).hour, 12);
  equal(autumn.add({ days: 1 }).epochNanoseconds - autumn.epochNanoseconds,
        25n * 3600n * 1000000000n);
  equal(autumn.add({ hours: 24 }).hour, 11);
  for (const [local, earlier, later] of [
    ['2026-03-29T02:30', '2026-03-29T00:30:00Z', '2026-03-29T01:30:00Z'],
    ['2026-10-25T02:30', '2026-10-25T00:30:00Z', '2026-10-25T01:30:00Z'],
  ]) {
    for (const [disambiguation, expected] of [['earlier', earlier], ['later', later]]) {
      equal(Temporal.ZonedDateTime.from(`${local}[${zone}]`, { disambiguation })
        .toInstant().toString(), expected);
    }
    assert.throws(() => Temporal.ZonedDateTime.from(`${local}[${zone}]`,
                                                 { disambiguation: 'reject' }), RangeError);
    checks++;
  }
  equal(Temporal.PlainDate.from('2024-02-29').add({ years: 1 }).toString(), '2025-02-28');
  equal(Temporal.PlainDate.from('2026-01-31').add({ months: 1 }).toString(), '2026-02-28');
  for (const [instant, nanos] of [
    ['1969-12-31T23:59:59.999999999Z', -1n],
    ['1970-01-01T00:00:00.000000001Z', 1n],
    ['2000-01-01T00:00:00Z', 946684800000000000n],
  ]) {
    equal(Temporal.Instant.from(instant).epochNanoseconds, nanos);
    equal(Temporal.Instant.fromEpochNanoseconds(nanos).toString(), instant);
  }
  equal(Temporal.PlainDate.from('2026-03-01').until('2026-04-01').days, 31);
  equal(spring.add({ days: 1 }).offset, '+02:00');
  equal(autumn.add({ days: 1 }).offset, '+01:00');
  return checks;
}

if (!isMainThread) {
  parentPort.postMessage(schedule());
} else {
  (async () => {
    const mainChecks = schedule();
    const workers = await Promise.all(Array.from({ length: 4 }, () => new Promise((resolve, reject) => {
      const worker = new Worker(__filename);
      let result;
      worker.once('message', (value) => { result = value; });
      worker.once('error', reject);
      worker.once('exit', (code) => {
        if (code === 0 && result === mainChecks) resolve(result);
        else reject(new Error(`Temporal worker exit ${code}, checks ${result}`));
      });
    })));
    console.log(JSON.stringify({ passed: true, zone: 'Europe/Oslo', workers: workers.length,
                               checks: mainChecks + workers.reduce((sum, n) => sum + n, 0) }));
  })().catch((error) => { console.error(error); process.exitCode = 1; });
}
