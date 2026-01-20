start_raft:
    # to deal with sock[0-5] and sockets not being cleaned up
    -rm sock1
    -rm sock2
    -rm sock3
    -rm sock4
    -rm sock5
    tmux new-session -d bash -c 'build/apps/raft_impl 1 ; sleep 1000'
    tmux split-window -h build/apps/raft_impl 2 
    tmux split-window -v build/apps/raft_impl 3 
    tmux select-pane -t 0
    tmux split-window -v build/apps/raft_impl 4 
    tmux split-window -h build/apps/raft_impl 5
    tmux attach
