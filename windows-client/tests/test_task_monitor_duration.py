import json
import tempfile
import time
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parents[1] / 'runtime'))
from task_monitor import TaskMonitor

class TaskDurationTests(unittest.TestCase):
    def test_completed_result_carries_nonzero_elapsed_ms(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); sessions=root/'sessions'; sessions.mkdir()
            path=sessions/'rollout-a.jsonl'
            def write(payload):
                with path.open('a',encoding='utf-8') as f:
                    f.write(json.dumps({'type':'event_msg','payload':payload})+'\n')
            path.write_text(json.dumps({'type':'session_meta','payload':{'id':'a'}})+'\n',encoding='utf-8')
            monitor=TaskMonitor(sessions,root/'state.json'); monitor.poll(force=True)
            write({'type':'task_started','thread_id':'a','turn_id':'t'})
            monitor.poll(force=True); time.sleep(0.02)
            write({'type':'task_complete','thread_id':'a','turn_id':'t','message':'done'})
            monitor.poll(force=True)
            result=monitor.pending('a')
            self.assertIsNotNone(result)
            self.assertGreater(result.get('elapsed_ms',0),0)

if __name__ == '__main__': unittest.main()
