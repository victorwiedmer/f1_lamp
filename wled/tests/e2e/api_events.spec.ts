/**
 * E2E tests – /api/events (live event log)
 *
 * Run: npx playwright test tests/e2e/api_events.spec.ts
 */
import { test, expect } from '@playwright/test';

test.describe('GET /api/events', () => {
  test('returns HTTP 200 with JSON', async ({ request }) => {
    const res = await request.get('/api/events');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toContain('application/json');
  });

  test('body has count and events array', async ({ request }) => {
    const body = await (await request.get('/api/events')).json();
    expect(typeof body.count).toBe('number');
    expect(Array.isArray(body.events)).toBe(true);
    expect(body.events.length).toBe(body.count);
  });

  test('each event has epoch, category and message', async ({ request }) => {
    const body = await (await request.get('/api/events')).json();
    for (const ev of body.events as any[]) {
      expect(typeof ev.epoch).toBe('number');
      expect(typeof ev.category).toBe('string');
      expect(typeof ev.message).toBe('string');
    }
  });
});

test.describe('POST /api/test_event', () => {
  test('injects a test event and it appears in /api/events', async ({ request }) => {
    // Inject
    const post = await request.post('/api/test_event', {
      data: { category: 'PW', message: 'playwright-test-event' },
    });
    expect(post.status()).toBeLessThan(300);

    // Verify
    const body  = await (await request.get('/api/events')).json();
    const found = (body.events as any[]).some(
      (e: any) => e.message === 'playwright-test-event',
    );
    expect(found).toBe(true);
  });
});
