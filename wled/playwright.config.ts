import { defineConfig, devices } from '@playwright/test';

/**
 * F1 Lamp E2E tests.
 *
 * Target device is resolved from the F1LAMP_HOST environment variable,
 * falling back to the mDNS hostname used in production.
 *
 * Usage:
 *   F1LAMP_HOST=192.168.1.42 npx playwright test
 *   npx playwright test                          # uses f1lamp.local
 */

const host = process.env.F1LAMP_HOST ?? 'f1lamp.local';

export default defineConfig({
  testDir: './tests/e2e',
  timeout: 30_000,
  expect: { timeout: 10_000 },
  retries: 1,
  use: {
    baseURL: `http://${host}`,
    /* No browser – all F1 Lamp tests are API / HTTP level only.
       Change to 'chromium' if you need real browser rendering tests. */
    ignoreHTTPSErrors: true,
  },
  projects: [
    {
      name: 'api',
      use: { ...devices['Desktop Chrome'] },
    },
  ],
});
