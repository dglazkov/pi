const { createHash } = require("node:crypto");
const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");

try {
	const helper = require(process.argv[2]);
	const value = helper[process.argv[3]]();
	const bytes = typeof value === "string" || Buffer.isBuffer(value) ? Buffer.from(value) : undefined;
	console.log(JSON.stringify({
		ok: true,
		value: value === null || (typeof value === "string" && value.length <= 100) ? value : undefined,
		unavailable: value === undefined || undefined,
		length: bytes?.length,
		hash: bytes && createHash("sha256").update(bytes).digest("hex"),
	}));
} catch (error) {
	console.log(JSON.stringify({ ok: false, error: String(error) }));
} finally {
	// Check before Node exits: process exit must not hide a leaked child or zombie.
	assert.equal(readFileSync(`/proc/self/task/${process.pid}/children`, "utf8").trim(), "");
}
