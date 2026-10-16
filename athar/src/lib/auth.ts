import crypto from 'node:crypto';
import { cookies } from 'next/headers';
import { db, type Plan, type User } from './db';

const SESSION_COOKIE = 'athar_session';
const SESSION_DAYS = 30;
const SCRYPT_KEYLEN = 64;

export function hashPassword(password: string): string {
  const salt = crypto.randomBytes(16);
  const derived = crypto.scryptSync(password, salt, SCRYPT_KEYLEN);
  return `scrypt$${salt.toString('hex')}$${derived.toString('hex')}`;
}

export function verifyPassword(password: string, stored: string): boolean {
  const parts = stored.split('$');
  if (parts.length !== 3 || parts[0] !== 'scrypt') return false;
  const salt = Buffer.from(parts[1], 'hex');
  const expected = Buffer.from(parts[2], 'hex');
  const derived = crypto.scryptSync(password, salt, expected.length);
  return crypto.timingSafeEqual(derived, expected);
}

export function createUser(email: string, password: string): User {
  const handle = db();
  const normalized = email.trim().toLowerCase();
  const now = new Date().toISOString();
  const result = handle
    .prepare('INSERT INTO users (email, password_hash, plan, created_at) VALUES (?, ?, ?, ?)')
    .run(normalized, hashPassword(password), 'free', now);
  return {
    id: Number(result.lastInsertRowid),
    email: normalized,
    password_hash: '',
    plan: 'free',
    created_at: now
  };
}

export function findUserByEmail(email: string): User | null {
  const row = db()
    .prepare('SELECT id, email, password_hash, plan, created_at FROM users WHERE email = ?')
    .get(email.trim().toLowerCase()) as User | undefined;
  return row ?? null;
}

export function findUserById(id: number): User | null {
  const row = db()
    .prepare('SELECT id, email, password_hash, plan, created_at FROM users WHERE id = ?')
    .get(id) as User | undefined;
  return row ?? null;
}

export async function createSession(userId: number): Promise<string> {
  const id = crypto.randomBytes(32).toString('base64url');
  const now = Date.now();
  const expires = new Date(now + SESSION_DAYS * 864e5).toISOString();
  db()
    .prepare('INSERT INTO sessions (id, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)')
    .run(id, userId, new Date(now).toISOString(), expires);
  const jar = await cookies();
  jar.set(SESSION_COOKIE, id, {
    httpOnly: true,
    sameSite: 'lax',
    secure: process.env.NODE_ENV === 'production',
    path: '/',
    expires: new Date(expires)
  });
  return id;
}

export async function destroySession(): Promise<void> {
  const jar = await cookies();
  const id = jar.get(SESSION_COOKIE)?.value;
  if (id) db().prepare('DELETE FROM sessions WHERE id = ?').run(id);
  jar.delete(SESSION_COOKIE);
}

export async function currentUser(): Promise<User | null> {
  const jar = await cookies();
  const id = jar.get(SESSION_COOKIE)?.value;
  if (!id) return null;
  const row = db()
    .prepare(
      `SELECT u.id, u.email, u.password_hash, u.plan, u.created_at
         FROM sessions s JOIN users u ON u.id = s.user_id
        WHERE s.id = ? AND s.expires_at > ?`
    )
    .get(id, new Date().toISOString()) as User | undefined;
  return row ?? null;
}

/**
 * A stable, non-reversible per-client key for the anonymous daily budget.
 * Salted so the raw IP is never stored.
 */
export function anonKey(ip: string | null): string {
  const salt = process.env.ATHAR_ANON_SALT || 'athar-dev-salt';
  return crypto.createHmac('sha256', salt).update(ip || 'unknown').digest('hex').slice(0, 32);
}

export function clientIp(headers: Headers): string | null {
  const fwd = headers.get('x-forwarded-for');
  if (fwd) return fwd.split(',')[0].trim();
  return headers.get('x-real-ip');
}

export function planOf(user: User | null): Plan {
  return user?.plan ?? 'free';
}
