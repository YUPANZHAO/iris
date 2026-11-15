import { expect, open, test } from "nsh/test";

test("mounts the Iris conference shell", async () => {
  const app = await open(".");
  expect(app.texts()).toContain("Secure SIP video");
  expect(app.texts()).toContain("SIP video conference");
  expect(app.texts()).toContain("Add account");
});
