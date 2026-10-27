FROM ubuntu:24.04
COPY install.sh /tmp/install.sh
RUN bash /tmp/install.sh && apt-get install -y -qq postgresql-client jq openssl nodejs && rm -rf /var/lib/apt/lists/*
WORKDIR /challenge
CMD ["bash"]
