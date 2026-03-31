FROM rust:1.78
WORKDIR /app
COPY Cargo.toml ./
COPY src ./src
RUN cargo build --release
EXPOSE 8088
CMD ["./target/release/agent-executor"]
