# Lint as: python3
#
# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Tests of the synthesis service: client and dummy server."""

import subprocess

import portpicker

from google.protobuf import text_format
from absl.testing import absltest
from xls.common import runfiles
from xls.synthesis import synthesis_pb2

CLIENT_PATH = runfiles.get_path('xls/synthesis/synthesis_client_main')
SERVER_PATH = runfiles.get_path('xls/synthesis/yosys/yosys_server_main')
YOSYS_PATH = runfiles.get_path('xls/synthesis/yosys/bogusys')
NEXTPNR_PATH = runfiles.get_path('xls/synthesis/yosys/nextpbr')

VERILOG = """
module main(
  input wire [31:0] x,
  input wire [31:0] y,
  output wire [31:0] out
);
  assign out = x + y;
endmodule
"""


class SynthesisServerTest(absltest.TestCase):

  def _start_server(self):
    port = portpicker.pick_unused_port()
    proc = subprocess.Popen([
        runfiles.get_path(SERVER_PATH), f'--port={port}',
        f'--yosys_path={YOSYS_PATH}', f'--nextpnr_path={NEXTPNR_PATH}'
    ])
    return port, proc

  def test_slack(self):
    port, proc = self._start_server()

    verilog_file = self.create_tempfile(content=VERILOG)

    response_text = subprocess.check_output(
        [CLIENT_PATH, verilog_file.full_path, f'--port={port}',
         '--ghz=1.0']).decode('utf-8')

    response = text_format.Parse(response_text, synthesis_pb2.CompileResponse())
    # The response is generated by parsing testdata/nextpnr.out.
    self.assertEqual(response.max_frequency_hz, 180280000)

    proc.terminate()
    proc.wait()


if __name__ == '__main__':
  absltest.main()
