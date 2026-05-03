/**
 * E2E tests – /api/nextrace and /api/calendar endpoints
 *
 * Run: npx playwright test tests/e2e/api_calendar.spec.ts
 */
import { test, expect } from '@playwright/test';

test.describe('GET /api/nextrace', () => {
  test('returns HTTP 200 with JSON content-type', async ({ request }) => {
    const res = await request.get('/api/nextrace');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('application/json');
  });

  test('body has expected structure when data is present', async ({ request }) => {
    const body = await (await request.get('/api/nextrace')).json();

    if (!body.hasData) {
      // No calendar loaded – that's valid; just check the flag is boolean
      expect(typeof body.hasData).toBe('boolean');
      return;
    }

    expect(typeof body.label).toBe('string');
    expect(body.label.length).toBeGreaterThan(0);

    expect(typeof body.raceDate).toBe('string');
    // Basic ISO date pattern: YYYY-MM-DD
    expect(body.raceDate).toMatch(/^\d{4}-\d{2}-\d{2}$/);

    expect(typeof body.daysUntil).toBe('number');
    expect(typeof body.weekendActive).toBe('boolean');
  });
});

test.describe('GET /api/calendar', () => {
  test('returns HTTP 200', async ({ request }) => {
    const res = await request.get('/api/calendar');
    expect(res.status()).toBe(200);
  });

  test('body is an array of race entries', async ({ request }) => {
    const body = await (await request.get('/api/calendar')).json();
    // May return an empty array if no custom calendar is loaded
    expect(Array.isArray(body)).toBe(true);

    if (body.length > 0) {
      const entry = body[0];
      expect(entry).toHaveProperty('name');
      expect(entry).toHaveProperty('raceDate');
    }
  });
});
