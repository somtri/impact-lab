'use strict';
// Headless smoke test (PLAN.md Stage 5 step 3, item 5 of brief 022-wasm): load the module, seed
// a ladder, inject a marketable order, assert fills. Run: node demo/wasm/test/smoke.js
// (after building demo/wasm/build/impact_wasm.js -- see demo/wasm/README.md).

const path = require('path');
const factory = require(path.join(__dirname, '..', 'build', 'impact_wasm.js'));

function fail(msg) {
    throw new Error(msg);
}

factory().then((Module) => {
    const book = new Module.Book(10n, 1000n); // BTCUSDT-style scale from types.hpp's own example

    // Seed a small synthetic ladder: 3 bid levels, 3 ask levels around a 100000-tick mid.
    book.seedLevel(0, 99990n, 10n);
    book.seedLevel(0, 99980n, 20n);
    book.seedLevel(0, 99970n, 30n);
    book.seedLevel(1, 100010n, 10n);
    book.seedLevel(1, 100020n, 20n);
    book.seedLevel(1, 100030n, 30n);

    const before = book.topN(10);
    if (before.bids.length !== 3 || before.asks.length !== 3) {
        fail('seedLevel: expected 3 levels per side, got ' +
            before.bids.length + ' bids / ' + before.asks.length + ' asks');
    }

    // Inject a marketable buy for 15 lots: walks the ask ladder, must fill fully (15 <= 10+20).
    const fills = book.injectMarketable(0, 15n);
    if (fills.length === 0) fail('injectMarketable produced no fills');
    let filled = 0n;
    for (const f of fills) filled += f.size;
    if (filled !== 15n) fail('expected 15 lots filled, got ' + filled.toString());

    const after = book.topN(10);
    if (after.asks.length === 0 || after.asks[0].price !== 100020n) {
        fail('best ask after the marketable buy should be 100020, got ' +
            JSON.stringify(after.asks[0], (k, v) => typeof v === 'bigint' ? v.toString() : v));
    }
    if (book.fillCount() !== 2n) {
        fail('expected 2 fills (100010 level consumed, 100020 level partly hit), got ' +
            book.fillCount().toString());
    }
    const expectedCost = 100010n * 10n + 100020n * 5n;
    if (book.cumulativeCost() !== expectedCost) {
        fail('cumulativeCost mismatch: expected ' + expectedCost.toString() +
            ', got ' + book.cumulativeCost().toString());
    }

    // injectMarketable must never leave a resting order: asking for far more than remains
    // clears every ask level and cancels the unfilled remainder rather than resting it.
    const fills2 = book.injectMarketable(0, 1000n);
    let filled2 = 0n;
    for (const f of fills2) filled2 += f.size;
    if (filled2 !== 45n) { // remaining ask liquidity: (20 - 5) + 30
        fail('expected the remaining 45 lots of ask liquidity, got ' + filled2.toString());
    }
    const afterAll = book.topN(10);
    if (afterAll.asks.length !== 0) {
        fail('ask side should be empty after consuming all liquidity, got ' +
            JSON.stringify(afterAll.asks, (k, v) => typeof v === 'bigint' ? v.toString() : v));
    }
    console.log('SMOKE PASS: seedLevel, injectMarketable, topN, fillCount, cumulativeCost all verified');
}).catch((err) => {
    console.error('SMOKE FAIL:', err);
    process.exitCode = 1;
});
