/**
 * E2E tests – /api/status endpoint
 *
 * Requires a live F1 Lamp device.
 * Set F1LAMP_HOST env var to override the default mDNS address.
 *
 * Run: npx playwright test tests/e2e/api_status.spec.ts
 */
import { test, expect } from '@playwright/test';

test.describe('GET /api/status', () => {
  test('returns HTTP 200 with correct Content-Type', async ({ request }) => {
    const res = await request.get('/api/status');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('application/json');
  });

  test('body contains required fields', async ({ request }) => {
    const res  = await request.get('/api/status');
    const body = await res.json();

    // Connection + state
    expect(typeof body.state).toBe('number');
    expect(typeof body.connected).toBe('boolean');

    // Network
    expect(typeof body.ssid).toBe('string');
    expect(typeof body.ip).toBe('string');
    expect(typeof body.rssi).toBe('number');

    // System
    expect(typeof body.heap).toBe('number');
    expect(body.heap).toBeGreaterThan(0);

    // Brightness + effect
    expect(typeof body.bri).toBe('number');
    expect(body.bri).toBeGreaterThanOrEqual(1);
    expect(body.bri).toBeLessThanOrEqual(255);

    expect(body).toHaveProperty('eff');
  });

  test('state is a valid track-status index (0–7)', async ({ request }) => {
    const body = await (await request.get('/api/status')).json();
    expect(body.state).toBeGreaterThanOrEqual(0);
    expect(body.state).toBeLessThanOrEqual(7);
  });

  test('rampPct is in range 0–100', async ({ request }) => {
    const body = await (await request.get('/api/status')).json();
    expect(body.rampPct).toBeGreaterThanOrEqual(0);
    expect(body.rampPct).toBeLessThanOrEqual(100);
  });
});
