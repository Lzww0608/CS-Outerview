
use futures::executor::block_on;
use futures::future::join_all;
use futures_timer::Delay;
use std::env;
use std::time::Duration;

const MAX_TERMS: usize = 187; // F(186) still fits in u128

fn fibonacci(index: usize) -> u128 {
    match index {
        0 => 0,
        1 => 1,
        _ => {
            let mut a = 0u128;
            let mut b = 1u128;
            for _ in 2..=index {
                let next = a + b;
                a = b;
                b = next;
            }
            b
        }
    }
}

async fn fibonacci_async(index: usize) -> u128 {
    // Add an async boundary so multiple tasks can be awaited together.
    Delay::new(Duration::from_millis(1)).await;
    fibonacci(index)
}

async fn fibonacci_series_async(n: usize) -> Vec<u128> {
    let tasks = (0..n).map(fibonacci_async);
    join_all(tasks).await
}

fn parse_n() -> Result<usize, String> {
    let Some(arg) = env::args().nth(1) else {
        return Err("用法: cargo run -- <n>".to_string());
    };

    let n: usize = arg
        .parse()
        .map_err(|_| "n 必须是正整数，例如: cargo run -- 10".to_string())?;

    if n == 0 {
        return Err("n 必须大于 0".to_string());
    }

    if n > MAX_TERMS {
        return Err(format!(
            "n 过大（最大 {}），再大将导致 u128 溢出",
            MAX_TERMS
        ));
    }

    Ok(n)
}

fn main() {
    let n = match parse_n() {
        Ok(value) => value,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(1);
        }
    };

    let values = block_on(fibonacci_series_async(n));

    println!("Fibonacci 前 {n} 项:");
    for (i, value) in values.iter().enumerate() {
        println!("F({i}) = {value}");
    }
}
