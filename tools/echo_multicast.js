'use strict';
const dgram  = require('dgram');
const config = require('./config');

const echo_cmd = new Buffer([5]);
const server   = dgram.createSocket('udp4');

function sendEchoCmd () {
  server.send(
    echo_cmd, 0, echo_cmd.length,
    config.port, config.address.multicast,
    function (err) {
      if(err) console.error('mulitcast error: ', err);
      else console.log('sent echo to ' + config.address.multicast + ':' + config.port);
    }
  );
}

server.on('message', function (msg, info) {
  console.log('' + info.address + ':', msg);
});
server.on('listening', function () {
  sendEchoCmd();
  setInterval(sendEchoCmd, 5000);
});
server.bind(config.port, config.address.interface);
