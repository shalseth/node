import type { Row } from './types.ts';
export function total<T extends Row>(rows: readonly T[]): number {
  return rows.reduce((sum: number, row: T): number => sum + row.value * row.weight, 0);
}
