#include "coding_flow.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    coding_flow_t flow;
    coding_flow_init(&flow);
    coding_flow_set_connected(&flow, true);
    coding_flow_set_done(&flow, "completed result");
    coding_flow_set_connected(&flow, false);
    assert(flow.stage == CODING_STAGE_DONE);
    assert(strcmp(flow.task_status, "completed result") == 0);
    coding_flow_set_connected(&flow, true);
    assert(flow.stage == CODING_STAGE_DONE);
    coding_flow_set_error(&flow, "failed result");
    coding_flow_set_connected(&flow, false);
    assert(flow.stage == CODING_STAGE_ERROR);
    coding_flow_set_connected(&flow, true);
    assert(flow.stage == CODING_STAGE_ERROR);
    return 0;
}
