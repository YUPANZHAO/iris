import { expect, open, test } from "nsh/test";

test("opens the dashboard with navigation and no call stage", async () => {
  const app = await open(".");
  expect(app.texts()).toContain("Dashboard");
  expect(app.texts()).toContain("Dialer");
  expect(app.texts()).toContain("Settings");
  expect(app.texts()).toContain("SIP listener");
  expect(app.texts()).toContain("Active calls");
  expect(app.texts().includes("Hang up")).toBe(false);
  expect(app.texts().includes("Show self-view")).toBe(false);

  await app.press("role:button[Dialer]");
  await app.waitFor("role:button[Delete]");
  expect(app.texts()).toContain("#");

  await app.press("role:button[Settings]");
  await app.waitFor("role:button[Add account]");
  expect(app.texts()).toContain("Show self-view");

  await app.press("role:button[Dashboard]");
  await app.waitFor("SIP listener");
});
