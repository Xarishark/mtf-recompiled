"""Which steps run, in what order, and the bookkeeping around them: fresh install, resume and update are one rule.

Each step has a key: a fingerprint of everything its output depends on, computed before anything runs (a step's key
includes the keys of the steps it reads from, so a change upstream reaches every step below it). A step is skipped
when mtf-install.json records it with the same key and its output is still there (Step.current). Otherwise it runs. A
transient step (its output lives in the temp folder and only later steps read it, Step.readers) runs only when one of
those steps runs. `always` steps (check, finish) run every time.

A failed or cancelled run leaves the finished steps recorded; the next run starts at the first step that is not.
"""
import datetime
import traceback

from . import state as statefile


class InstallError(Exception):
    """A failure with a message id (installer/text/en.json) and its parameters."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


class Step:
    id = ''
    transient = False
    readers = ()
    always = False

    def key(self, ctx):
        """Fingerprint of the step's inputs; may read ctx.keys of earlier steps."""
        return ''

    def current(self, ctx, record):
        """True when the recorded output is still in place (cheap checks only)."""
        return True

    def run(self, ctx):
        """Do the work. Returns fields to record with the step."""
        return {}


class Context:
    def __init__(self, layout, dest, temp, reporter, options, state=None, forced=(), only=()):
        self.layout, self.dest, self.temp, self.reporter, self.options = layout, dest, temp, reporter, options
        self.state = state or statefile.empty(layout.release)
        self.mode = 'install' if state is None else 'resume' if state.get('release') == layout.release else 'update'
        self.keys, self.runs, self.reasons, self.steps = {}, {}, {}, []
        self.forced, self.only = set(forced), set(only)

    def record(self, step_id):
        return self.state['steps'].get(step_id)

    def will_run(self, step_id):
        return self.runs.get(step_id, False)

    def save(self):
        self.dest.mkdir(parents=True, exist_ok=True)
        statefile.save(self.dest, self.state)

    def plan(self):
        """Fill keys, runs and reasons."""
        for s in self.steps:
            self.keys[s.id] = s.key(self)
        for s in self.steps:
            rec = self.record(s.id)
            if s.always or s.id in self.forced:
                self.runs[s.id] = True
            else:
                self.runs[s.id] = not (rec and rec.get('key') == self.keys[s.id] and s.current(self, rec))
            self.reasons[s.id] = '' if self.runs[s.id] else 'current'
        # last to first: a transient step asked for by another transient step is known before the one that feeds it
        for s in reversed(self.steps):
            if s.transient and self.runs[s.id] and s.id not in self.forced and not any(self.runs[r] for r in s.readers):
                self.runs[s.id], self.reasons[s.id] = False, 'not_needed'
        if self.only:
            for s in self.steps:
                if s.id not in self.only and not s.always:
                    self.runs[s.id], self.reasons[s.id] = False, 'not_needed'


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def run_steps(steps, ctx):
    """Run the plan. Returns 'ok', 'failed' or 'cancelled'; events and the log say why."""
    rep = ctx.reporter
    ctx.steps = steps
    current = None
    try:
        ctx.plan()
        rep.emit('plan', run=[s.id for s in steps if ctx.runs[s.id]],
                 skip={s.id: ctx.reasons[s.id] for s in steps if not ctx.runs[s.id]})
        for s in steps:
            if not ctx.runs[s.id]:
                rep.step(s.id, 'skipped', reason=ctx.reasons[s.id])
                continue
            current = s.id
            rep.step(s.id, 'started')
            t0 = rep.elapsed()
            fields = s.run(ctx) or {}
            seconds = round(rep.elapsed() - t0, 1)
            if not s.always:
                ctx.state['steps'][s.id] = {'key': ctx.keys[s.id], 'release': ctx.layout.release,
                                            'finished': now_utc(), 'seconds': seconds, **fields}
            ctx.save()   # the first save, after the check, creates the install folder
            rep.step(s.id, 'done', seconds=seconds)
            current = None
        ctx.state['release'] = ctx.layout.release
        ctx.save()
        return 'ok'
    except InstallError as e:
        rep.error(e.id, e.params, step=current)
        return 'failed'
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)}, step=current)
        return 'failed'
    except KeyboardInterrupt:
        rep.log(f'cancelled during {current}')
        return 'cancelled'
    except Exception as e:
        rep.log(traceback.format_exc())
        rep.error('internal', {'exception': type(e).__name__, 'detail': str(e)}, step=current)
        return 'failed'
