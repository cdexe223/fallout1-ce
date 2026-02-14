#ifndef FALLOUT_GAME_CLI_H_
#define FALLOUT_GAME_CLI_H_

namespace fallout {

bool cli_is_enabled();
void cli_set_enabled(bool enabled);
int cli_init();
void cli_exit();
void cli_process_bk();

} // namespace fallout

#endif /* FALLOUT_GAME_CLI_H_ */
