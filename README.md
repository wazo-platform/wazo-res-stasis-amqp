AMQP support for stasis in Asterisk
-----------------------------------

How does it works
-----------------

1. Client Sends request application via ARI
2. Asterisk create application and registers it to Stasis
3. Receiver receives events
4. Client receives response via AMQP

```
 +--- stasis application
 |
 v                     |                                  |
[client]-------------->|  topic --> (*)--> callback---+   |   
                       |                    |    ^    |   |
                       |                    |    |    +-->| ==== event ==> [RabbitMQ Exchange(s)]
                       |                    v    |        |
        <--------------|       event >>> Stasis -+        |
```

How to install
--------------

To build the module you will need the following dependencies

* asterisk-dev
* wazo-res-amqp-dev
* librabbitmq-dev

To install

    make
    make install
    make samples

Configure the file in /etc/asterisk/stasis_amqp.conf

You need to have res_amqp.so loaded.

Please restart asterisk before loading res_stasis_amqp.so for the documentation.

To load module

    CLI> module load res_stasis_amqp.so

How to use
----------

# ARI

on your asterisk dialplan. For an application named 'bar'

    exten = 6001,1,NoOp() 
     same = n,Answer()
     same = n,Stasis(bar) ; this will generate events which will be forwarded to stasis (websocket or AMQP)
     same = n,Hangup()

To activate the events on AMQP for your ARI application you need to use the ARI REST API endpoint.

Create a Stasis Application named 'bar'

    POST with applicationName=bar

This will create an internal application that will send events to AMQP

To delete the application created above

    DELETE with applicationName=bar

This will delete the application, events will no longer be sent to AMQP

Event is push on this routing key `stasis.app.<app name>`

# AMI

You don't need to anything for the configuration, all AMI events is pushed by default on rabbitmq

Event is push on this routing key `stasis.ami.<event name>`

# Channels

You don't need to anything for the configuration, all channels events is pushed by default on rabbitmq

Event is push on this routing key `stasis.channel.<channel uniqueid>`
