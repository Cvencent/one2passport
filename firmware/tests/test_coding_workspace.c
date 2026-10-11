#include <assert.h>
#include "../main/coding_workspace.h"
int main(void) {
 coding_workspace_t *work = NULL;
 assert(coding_workspace_acquire(&work));
 coding_workspace_t *first = work;
 work->detail[0] = 'x';
 assert(coding_workspace_acquire(&work));
 assert(work == first && work->detail[0] == 'x');
 coding_workspace_release(&work);
 assert(work == NULL);
 coding_workspace_release(&work);
 assert(coding_workspace_acquire(&work));
 assert(work->detail[0] == 0 && work->render_detail[0] == 0);
 coding_workspace_release(&work);
 return 0;
}
