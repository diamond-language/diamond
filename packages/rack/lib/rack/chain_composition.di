def rack_terminal_wrap(app)
  def terminal(request, context, forward)
    app(request, context)
  end
  terminal
end

# middlewares: Array of Callable[3]. app: Callable[2], the terminal
# handler. Returns a plain Array -- see the package-level comment
# (rack.di) for why that matters.
def rack_compose(middlewares, app)
  middlewares.concat([rack_terminal_wrap(app)])
end

def rack_run_chain(chain, index, request, context)
  if index >= chain.length()
    raise RuntimeError.new("rack: chain exhausted without a response " +
      "(a middleware called forward() more times than the chain has steps)")
  end
  current = chain[index]
  def forward(req, ctx)
    rack_run_chain(chain, index + 1, req, ctx)
  end
  current(request, context, forward)
end
