#!/bin/bash
. ./resources.sh
export CLIPBOARD_FORCETTY=1
start_test "Cut user provided text"

clipboard cut "Foobar"

item_is_in_cb 0 rawdata.clipboard

clipboard paste

item_is_here rawdata.clipboard

item_is_not_in_cb 0 rawdata.clipboard