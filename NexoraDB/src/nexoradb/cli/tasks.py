"""Screen-owned async tasks: bounded admission, cancellation and stale UI guards."""

import asyncio


class ScreenTasks:
    def _init_tasks(self):
        self._tasks = {}
        self._closing = False
        self._leaving = False

    def _launch(self, name, factory):
        if self._closing or self._leaving or name in self._tasks:
            return False

        async def run():
            try:
                await factory()
            except asyncio.CancelledError:
                pass
            except Exception as exc:
                if not self._closing and not self._leaving:
                    self._task_error(name, exc)
            finally:
                self._tasks.pop(name, None)
                if not self._closing and not self._leaving:
                    self._busy_changed(name, False)

        self._tasks[name] = asyncio.create_task(run())
        self._busy_changed(name, True)
        return True

    async def _close_tasks(self):
        self._closing = True
        tasks = list(self._tasks.values())
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        self._tasks.clear()
