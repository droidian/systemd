/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "process-util.h"
#include "tests.h"
#include "umask-util.h"

int main(int argc, char *argv[]) {
        size_t n;
        mode_t u, t;

        test_setup_logging(LOG_DEBUG);

        u = umask(0111);

        n = 0;
        WITH_UMASK(0123) {
                assert_se(umask(000) == 0123);
                n++;

                assert_se(get_process_umask(0, &t) == 0);
                assert_se(t == 000);
        }

        assert_se(n == 1);
        assert_se(umask(u) == 0111);

        assert_se(get_process_umask(getpid_cached(), &t) == 0);
        assert_se(t == u);

        WITH_UMASK(0135) {
                assert_se(umask(000) == 0135);
                n++;

                assert_se(get_process_umask(0, &t) == 0);
                assert_se(t == 000);
        }

        assert_se(n == 2);
        assert_se(umask(0111) == u);

        assert_se(get_process_umask(0, &t) == 0);
        assert_se(t == 0111);

        WITH_UMASK(0315) {
                assert_se(umask(000) == 0315);
                n++;
                break;
        }

        assert_se(n == 3);
        assert_se(umask(u) == 0111);

        assert_se(get_process_umask(0, &t) == 0);
        assert_se(t == u);

        return EXIT_SUCCESS;
}
