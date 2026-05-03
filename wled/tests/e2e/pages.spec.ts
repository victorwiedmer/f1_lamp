/**
 * E2E tests – Static pages served from LittleFS
 *
 * Checks that the device serves its web UI pages with HTTP 200
 * and the expected HTML structure.
 *
 * Run: npx playwright test tests/e2e/pages.spec.ts
 */
import { test, expect } from '@playwright/test';

const PAGES = [
  { path: '/',              titleContains: 'F1 Lamp' },
  { path: '/sessions.html', titleContains: 'Sessions' },
  { path: '/settings.html', titleContains: 'Settings' },
  { path: '/effects.html',  titleContains: 'Effects' },
  { path: '/features.html', titleContains: 'Features' },
];

for (const { path, titleContains } of PAGES) {
  test(`GET ${path} → 200 with correct title`, async ({ page }) => {
    const res = await page.goto(path);
    expect(res?.status()).toBe(200);
    await expect(page).toHaveTitle(new RegExp(titleContains, 'i'));
  });
}

test.describe('index.html structure', () => {
  test('has stateBox element', async ({ page }) => {
    await page.goto('/');
    const stateBox = page.locator('#stateBox');
    await expect(stateBox).toBeVisible({ timeout: 5000 });
  });

  test('status card shows network info after load', async ({ page }) => {
    await page.goto('/');
    // Give the page's JS time to fetch /api/status
    await page.waitForTimeout(2000);
    const wifiInfo = page.locator('#wifiInfo');
    const text = await wifiInfo.textContent();
    // Should no longer show "Loading…" once data arrives
    expect(text).not.toBe('Loading…');
  });
});

test.describe('sessions.html structure', () => {
  test('has Load button', async ({ page }) => {
    await page.goto('/sessions.html');
    const btn = page.locator('#sessBtn');
    await expect(btn).toBeVisible();
    await expect(btn).toHaveText(/load/i);
  });
});
