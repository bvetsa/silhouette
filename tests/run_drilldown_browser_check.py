#!/usr/bin/env python3
"""Optional real-browser acceptance via Playwright CLI; no JS build dependency.

python3 tests/run_drilldown_browser_check.py build/tests/drilldown_browser_fixture \
    --cli /path/to/playwright_cli.sh --output build/browser
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--cli", default="playwright-cli")
    parser.add_argument("--output", type=Path, default=Path("build/browser"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    session = f"silhouette-acceptance-{int(time.time())}"
    fixture = None
    report = []
    (output / "report.json").write_text(json.dumps({"passed": False, "checks": []}))
    (output / "browser-check.log").write_text("")

    def cli(*commands):
        result = subprocess.run(
            [args.cli, f"--session={session}", *commands], cwd=output,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        with (output / "browser-check.log").open("a") as log:
            log.write(result.stdout + "\n")
        # The CLI reports evaluation failures in its response, sometimes with
        # a successful process exit status. Treat either form as failure.
        if result.returncode or "### Error" in result.stdout:
            raise RuntimeError(result.stdout)
        return result.stdout

    def check(name, code):
        cli("snapshot")
        result = cli("run-code", "async page => { " + code + " }")
        report.append({"check": name, "result": result})
        print("PASS", name, flush=True)

    def start():
        nonlocal fixture
        fixture = subprocess.Popen([str(args.fixture.resolve())], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=(output / "fixture.log").open("a"), text=True)
        ready = fixture.stdout.readline().strip()
        if not ready.startswith("READY "):
            raise RuntimeError("Browser fixture did not start: " + ready)
        return ready.removeprefix("READY ")

    def command(value):
        fixture.stdin.write(value + "\n")
        fixture.stdin.flush()
        result = fixture.stdout.readline().strip()
        if not result.startswith("DONE " + value):
            raise RuntimeError("Fixture command failed: " + result)

    def stop():
        nonlocal fixture
        if fixture and fixture.poll() is None:
            fixture.stdin.write("stop\n"); fixture.stdin.flush()
            fixture.wait(timeout=15)
        fixture = None

    try:
        url = start(); command("seed")
        cli("open", url)
        check("service, edge, gap, keyboard, and exact popup", """
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).waitFor();
            await page.getByRole('button', {name:'Supporting traces for frontend → checkout',exact:true}).press('Space');
            await page.waitForFunction(() => document.querySelector('#fact-label').textContent === 'frontend → checkout');
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).press('Enter');
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 2);
            await page.getByRole('button', {name:'Supporting traces for Missing parent',exact:true}).click();
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 1);
            await page.locator('#traces button').click();
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.startsWith('1 retained span'));
            if (!await page.locator('#graph .dimmed').count()) throw Error('No unrelated topology de-emphasis');
            if (await page.locator('#graph script, #graph foreignObject, #graph [href]').count()) throw Error('Active SVG content');
            const inspected = page.locator('#trace-tree summary').first();
            await inspected.click(); await inspected.focus();
            return {selected:await page.locator('#trace-summary').textContent()};
        """)
        command("parent")
        check("late parent updates retained selection after original support disappears", """
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.startsWith('2 retained spans')
                && document.querySelector('#support-status').textContent.includes('No supporting traces'));
            const root = page.locator('#trace-tree > ul > li > details > summary').first();
            if (!(await root.textContent()).includes('frontend')) throw Error('Late parent did not replace root');
            await page.waitForFunction(() => document.querySelectorAll('#trace-tree details').length === 2);
            if (!(await root.locator('..').getAttribute('open') !== null)) throw Error('New ancestor did not preserve inspected branch');
            if (!(await page.locator('#trace-tree details details').getAttribute('open') !== null)) throw Error('Expanded child collapsed');
            if (!(await page.evaluate(() => document.activeElement.textContent.includes('checkout')))) throw Error('Inspected span lost focus');
            await page.waitForFunction(() => !document.querySelector('[aria-label="Supporting traces for Missing parent"]'));
            await page.evaluate(() => {
                document.querySelector('#graph svg').dataset.quietFixture = 'unchanged';
                window.__inspectedTree = document.querySelector('#trace-tree > ul');
            });
            return {liveParent:true};
        """)
        command("quiet")
        check("quiescence invalidates details without replacing SVG", """
            await page.waitForFunction(() => document.querySelector('#trace-status').textContent.startsWith('quiescent'));
            if (!await page.locator('#graph svg[data-quiet-fixture=unchanged]').count()) throw Error('Quiescence replaced SVG');
            if (!await page.evaluate(() => window.__inspectedTree === document.querySelector('#trace-tree > ul')
                && document.activeElement.textContent.includes('checkout'))) throw Error('Lifecycle update rebuilt inspection');
            return {quiescent:true};
        """)
        command("expire")
        check("eviction freezes browser snapshot", """
            await page.waitForFunction(() => document.querySelector('#trace-status').textContent.startsWith('No longer retained'));
            if (!(await page.locator('#trace-summary').textContent()).startsWith('2 retained spans')) throw Error('Snapshot discarded');
            return {frozen:true};
        """)
        command("reappear")
        check("reincarnation cannot replace frozen popup; close discards it", """
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).waitFor();
            if (!(await page.locator('#trace-status').textContent()).startsWith('No longer retained')
                || !(await page.locator('#trace-summary').textContent()).startsWith('2 retained spans'))
                throw Error('New incarnation replaced frozen snapshot');
            await page.getByRole('button', {name:'Close',exact:true}).click();
            await page.waitForFunction(() => document.querySelector('#trace-tree').textContent === '');
            if (!await page.evaluate(() => document.activeElement === document.querySelector('#support-heading')
                || document.activeElement.getAttribute('aria-label') === 'Supporting traces for Missing parent')) throw Error('Missing trigger fallback lost focus');
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).press('Enter');
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 1);
            await page.locator('#traces button').click();
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.startsWith('1 retained span'));
            await page.evaluate(() => {
                Object.defineProperty(navigator.clipboard, 'writeText', {configurable:true, writable:true, value:async value => { window.__copiedId = value; }});
            });
            await page.getByRole('button', {name:'Copy trace ID',exact:true}).click();
            await page.waitForFunction(() => document.querySelector('#copy-status').textContent === 'ID copied.');
            if (await page.evaluate(() => window.__copiedId) !== '00000000000000000000000000000001') throw Error('Wrong copied ID');
            await page.evaluate(() => { navigator.clipboard.writeText = async () => { throw Error('Clipboard denied'); }; });
            await page.getByRole('button', {name:'Copy trace ID',exact:true}).click();
            await page.waitForFunction(() => document.querySelector('#copy-status').textContent.startsWith('Clipboard unavailable.'));
            await page.getByRole('button', {name:'Close',exact:true}).click();
            await page.waitForFunction(() => document.querySelector('#trace-tree').textContent === '');
            if (!await page.locator('#traces button').evaluate(node => node === document.activeElement)) throw Error('Dialog did not return focus to trace row');
            return {freshIncarnation:true};
        """)
        command("pages")
        check("50-record paging", """
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 50);
            await page.getByRole('button', {name:'Load more traces',exact:true}).click();
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 52);
            if (await page.locator('#more').isVisible()) throw Error('Continuation remained after final page');
            return {loaded:52};
        """)
        command("parent")
        check("invalidation resets live pagination to newest page", """
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 50
                && document.querySelector('#traces button').textContent.includes('00000000…00000001'));
            return {refreshedFirstPage:true};
        """)
        command("large")
        check("complete large trace with bounded DOM expansion", """
            await page.getByRole('button', {name:'Supporting traces for batch-worker',exact:true}).click();
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 1);
            await page.locator('#traces button').click();
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.startsWith('10000 retained spans'));
            if (await page.locator('#trace-tree details').count() !== 100) throw Error('Initial DOM is not bounded');
            await page.getByRole('button', {name:'Show more (9900 remaining)',exact:true}).press('Enter');
            if (await page.locator('#trace-tree details').count() !== 200) throw Error('Root expansion discarded evidence');
            if (!await page.evaluate(() => document.activeElement === document.querySelectorAll('#trace-tree summary')[100])) throw Error('Show more lost keyboard focus');
            await page.locator('#trace-tree summary').nth(150).click();
            await page.locator('#trace-tree summary').nth(150).focus();
            await page.evaluate(() => { window.__largeTree = document.querySelector('#trace-tree > ul'); });
            return {retained:10000, visibleRoots:200};
        """)
        command("quiet")
        check("live refresh preserves loaded tree batches and keyboard focus", """
            await page.waitForFunction(() => document.querySelector('#trace-status').textContent.startsWith('quiescent'));
            if (!await page.evaluate(() => window.__largeTree === document.querySelector('#trace-tree > ul')
                && document.activeElement === document.querySelectorAll('#trace-tree summary')[150])) throw Error('Live refresh lost tree or focus');
            if (await page.locator('#trace-tree details').count() !== 200) throw Error('Loaded batch collapsed');
            await page.getByRole('button', {name:'Close',exact:true}).click();
            return {inspectionPreserved:true};
        """)
        command("seed")
        check("late detail response cannot overwrite new selection", """
            // Ignore AbortSignal deliberately, so request-generation protection
            // must work even when cancellation cannot prevent delivery.
            await page.evaluate(() => {
                const original = window.fetch;
                window.fetch = (input, init) => { const options = {...init}; delete options.signal; return original(input, options); };
            });
            let release, started;
            const gate = new Promise(resolve => { release = resolve; });
            const entered = new Promise(resolve => { started = resolve; });
            await page.route('**/query/trace/**', async route => {
                if (route.request().url().includes('00000000000000000000000000000001/')) {
                    const response = await route.fetch(); started(); await gate;
                    await route.fulfill({response});
                } else await route.continue();
            });
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).click();
            await page.waitForFunction(() => document.querySelector('#traces button')?.textContent.includes('00000000…00000001'));
            await page.locator('#traces button').first().click(); await entered;
            await page.getByRole('button', {name:'Close',exact:true}).click();
            await page.getByRole('button', {name:'Supporting traces for inventory',exact:true}).click();
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 1);
            await page.locator('#traces button').click();
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.endsWith('00000000000000000000000000000002'));
            const delivered = page.waitForResponse(response => response.url().includes('00000000000000000000000000000001/'));
            release(); await delivered;
            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
            if (!(await page.locator('#trace-summary').textContent()).endsWith('00000000000000000000000000000002')) throw Error('Late detail overwrote selection');
            await page.unroute('**/query/trace/**');
            await page.getByRole('button', {name:'Close',exact:true}).click();
            let firstGraph = true, graphEntered, releaseGraph;
            const graphStarted = new Promise(resolve => { graphEntered = resolve; });
            const graphGate = new Promise(resolve => { releaseGraph = resolve; });
            await page.exposeFunction('__waitGraphEntered', () => graphStarted);
            await page.exposeFunction('__releaseGraph', () => releaseGraph());
            await page.route('**/graph.svg*', async route => {
                if (firstGraph) {
                    firstGraph = false;
                    const response = await route.fetch();
                    graphEntered();
                    await graphGate;
                    await route.fulfill({response});
                } else await route.continue();
            });
            return {lateDetailIgnored:true};
        """)
        command("extra-a")
        check("hold old SVG response", """
            await page.evaluate(() => window.__waitGraphEntered());
            return {oldFrameHeld:true};
        """)
        command("extra-b")
        check("late SVG response cannot replace newer frame", """
            await page.getByRole('button', {name:'Supporting traces for shipping-b',exact:true}).waitFor();
            const delivered = page.waitForResponse(response => response.url().includes('/graph.svg'));
            await page.evaluate(() => window.__releaseGraph()); await delivered;
            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
            if (!await page.getByRole('button', {name:'Supporting traces for shipping-b',exact:true}).count()) throw Error('Old SVG replaced new frame');
            await page.unroute('**/graph.svg*');
            await page.route('**/graph.svg*', async route => {
                const response = await route.fetch();
                const body = (await response.text()).replace('</svg>',
                    '<script>window.__unsafeSvg=true</script><foreignObject><div>unsafe</div></foreignObject><image href="/should-not-fetch"/><use href="/should-not-fetch"/></svg>');
                await route.fulfill({response, body});
            });
            return {lateSvgIgnored:true};
        """)
        command("extra-c")
        check("inline SVG rejects active injected content", """
            await page.getByRole('button', {name:'Supporting traces for shipping-c',exact:true}).waitFor();
            if (await page.locator('#graph script, #graph foreignObject, #graph image, #graph use, #graph [href]').count())
                throw Error('SVG sanitizer retained active content');
            if (await page.evaluate(() => window.__unsafeSvg)) throw Error('SVG script executed');
            await page.unroute('**/graph.svg*');
            return {sanitized:true};
        """)

        # Final visual evidence uses fresh real state, without request interception.
        stop(); url = start(); command("seed"); cli("goto", url)
        paths = {name: str(output / name) for name in
                 ("desktop.png", "mobile.png", "desktop-dialog.png", "mobile-dialog.png")}
        check("desktop/mobile visual evidence and overflow", """
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).waitFor();
            await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).click();
            await page.waitForFunction(() => document.querySelectorAll('#traces button').length === 2);
            await page.setViewportSize({width:1440,height:900});
            const settle = async () => page.evaluate(() => { scrollTo(0,0); return new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))); });
            await settle();
            await page.screenshot({path:PATH_DESKTOP,fullPage:false});
            await page.setViewportSize({width:390,height:844});
            await settle();
            if (await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)) throw Error('Mobile page overflows');
            if (await page.getByRole('button', {name:'Supporting traces for checkout',exact:true}).locator('text').evaluate(node => node.getBoundingClientRect().height) < 12) throw Error('Mobile map label is too small');
            await page.screenshot({path:PATH_MOBILE,fullPage:false});
            await page.locator('#traces button').filter({hasText:'00000000…00000001'}).click();
            await page.waitForFunction(() => document.querySelector('#trace-summary').textContent.startsWith('1 retained span'));
            await settle();
            await page.screenshot({path:PATH_MOBILE_DIALOG,fullPage:false});
            await page.setViewportSize({width:1440,height:900});
            await settle();
            await page.screenshot({path:PATH_DESKTOP_DIALOG,fullPage:false});
            return {screenshots:true};
        """.replace("PATH_DESKTOP_DIALOG", json.dumps(paths["desktop-dialog.png"]))
           .replace("PATH_MOBILE_DIALOG", json.dumps(paths["mobile-dialog.png"]))
           .replace("PATH_DESKTOP", json.dumps(paths["desktop.png"]))
           .replace("PATH_MOBILE", json.dumps(paths["mobile.png"])))
        (output / "report.json").write_text(json.dumps({"passed": True, "checks": report}, indent=2))
        print("Browser acceptance passed; artifacts:", output, flush=True)
    finally:
        try:
            cli("close")
        finally:
            stop()


if __name__ == "__main__":
    main()
