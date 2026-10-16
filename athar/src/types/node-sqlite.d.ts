// Minimal ambient declaration for the experimental built-in SQLite module.
// Node 22 ships `node:sqlite`; @types/node coverage varies by patch release.
declare module 'node:sqlite' {
  export class DatabaseSync {
    constructor(path: string, options?: { open?: boolean; readOnly?: boolean });
    exec(sql: string): void;
    prepare(sql: string): StatementSync;
    close(): void;
  }
  export interface StatementSync {
    run(...params: unknown[]): { changes: number | bigint; lastInsertRowid: number | bigint };
    get(...params: unknown[]): unknown;
    all(...params: unknown[]): unknown[];
  }
}
