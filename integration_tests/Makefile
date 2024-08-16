test-setup: build-asterisk build-asterisk-refcounter

build-asterisk:
	docker build --no-cache -t wazoplatform/asterisk-amqp:local ..

build-asterisk-refcounter:
	docker build -t wazoplatform/asterisk-refcounter:local -f docker/Dockerfile-asterisk-refcounter .

test:
	pytest -x suite

.PHONY: test test-setup build-asterisk build-asterisk-refcounter
